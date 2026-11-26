#include "src/agent/meeting_tool_registry.h"
#include "src/agent/detail/meeting_tool_helpers.h"

#include "src/agent/agent_coordinator.h"
#include "src/core/call_invite_controller.h"
#include "src/core/contact_catalog_controller.h"
#include "src/core/meeting_catalog_controller.h"
#include "src/core/meeting_coordinator.h"
#include "src/core/presence_watch_controller.h"
#include "src/net/meeting_types.h"
#include "src/net/session_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimeZone>

#include <algorithm>
#include <cmath>
#include <memory>

namespace OpenMeeting {
using namespace tool_helpers;

namespace {

QString defaultSystemTimeZoneId() {
	const QString sys = QString::fromUtf8(QTimeZone::systemTimeZoneId());
	if (!sys.isEmpty() && QTimeZone(sys.toUtf8()).isValid()) {
		return sys;
	}
	return QStringLiteral("Asia/Shanghai");
}

QTimeZone resolveValidTimeZone(const QString &tzId, QString *outCanonicalId = nullptr) {
	const QString clean = tzId.trimmed().isEmpty() ? defaultSystemTimeZoneId() : tzId.trimmed();
	QTimeZone tz(clean.toUtf8());
	if (!tz.isValid()) {
		const QString fallback = defaultSystemTimeZoneId();
		tz = QTimeZone(fallback.toUtf8());
		if (outCanonicalId) {
			*outCanonicalId = fallback;
		}
		return tz;
	}
	if (outCanonicalId) {
		*outCanonicalId = clean;
	}
	return tz;
}

bool parseFlexibleTimestamp(
		const QJsonValue &val,
		const QString &timeZoneId,
		qint64 referenceTimeSeconds,
		qint64 &outSeconds,
		QString &outError,
		const QString &fieldName) {
	outSeconds = 0;
	if (val.isDouble()) {
		const double num = val.toDouble();
		if (!std::isfinite(num) || num <= 0) {
			outError = QStringLiteral("Invalid numeric timestamp for '%1'.").arg(fieldName);
			return false;
		}
		outSeconds = (num > 32503680000.0)
			? static_cast<qint64>(num / 1000.0)
			: static_cast<qint64>(num);
		return outSeconds > 0;
	}

	if (!val.isString()) {
		outError = QStringLiteral("Parameter '%1' must be an ISO-8601 string or Unix timestamp.").arg(fieldName);
		return false;
	}

	const QString raw = val.toString().trimmed();
	if (raw.isEmpty()) {
		outError = QStringLiteral("Parameter '%1' must not be empty.").arg(fieldName);
		return false;
	}

	bool numericOk = false;
	const qint64 directUnix = raw.toLongLong(&numericOk, 10);
	if (numericOk && directUnix > 0) {
		outSeconds = (directUnix > 32503680000LL) ? (directUnix / 1000LL) : directUnix;
		return true;
	}

	const QTimeZone tz = resolveValidTimeZone(timeZoneId);

	if (raw.size() == 10) {
		QDate date = QDate::fromString(raw, QStringLiteral("yyyy-MM-dd"));
		if (!date.isValid()) {
			date = QDate::fromString(raw, QStringLiteral("yyyy/MM/dd"));
		}
		if (date.isValid()) {
			QTime timeOfDay(9, 0, 0);
			if (referenceTimeSeconds > 0) {
				const QDateTime refDt = QDateTime::fromSecsSinceEpoch(referenceTimeSeconds, tz);
				if (refDt.isValid()) {
					timeOfDay = refDt.time();
				}
			}
			const QDateTime dt(date, timeOfDay, tz);
			if (dt.isValid()) {
				outSeconds = dt.toSecsSinceEpoch();
				return outSeconds > 0;
			}
		}
	}

	const int tPos = std::max(raw.indexOf(QLatin1Char('T')), raw.indexOf(QLatin1Char(' ')));
	const bool hasExplicitOffset = raw.endsWith(QLatin1Char('Z'), Qt::CaseInsensitive) ||
		(tPos > 0 && (raw.indexOf(QLatin1Char('+'), tPos) > tPos || raw.indexOf(QLatin1Char('-'), tPos) > tPos));
	if (hasExplicitOffset) {
		const QDateTime isoDt = QDateTime::fromString(raw, Qt::ISODate);
		if (isoDt.isValid()) {
			outSeconds = isoDt.toSecsSinceEpoch();
			return outSeconds > 0;
		}
	}

	static const QStringList kLocalFormats = {
		QStringLiteral("yyyy-MM-ddTHH:mm:ss"),
		QStringLiteral("yyyy-MM-ddTHH:mm"),
		QStringLiteral("yyyy-MM-dd HH:mm:ss"),
		QStringLiteral("yyyy-MM-dd HH:mm"),
		QStringLiteral("yyyy/MM/dd HH:mm:ss"),
		QStringLiteral("yyyy/MM/dd HH:mm"),
		QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz"),
	};
	for (const QString &fmt : kLocalFormats) {
		const QDateTime naive = QDateTime::fromString(raw, fmt);
		if (naive.isValid()) {
			const QDateTime localized(naive.date(), naive.time(), tz);
			if (localized.isValid()) {
				outSeconds = localized.toSecsSinceEpoch();
				return outSeconds > 0;
			}
		}
	}

	const QDateTime fallbackIso = QDateTime::fromString(raw, Qt::ISODate);
	if (fallbackIso.isValid()) {
		const QDateTime localized = fallbackIso.timeSpec() == Qt::LocalTime
			? QDateTime(fallbackIso.date(), fallbackIso.time(), tz)
			: fallbackIso;
		outSeconds = localized.toSecsSinceEpoch();
		if (outSeconds > 0) {
			return true;
		}
	}

	outError = QStringLiteral("Could not parse '%1' ('%2'). Use 'YYYY-MM-DDTHH:mm:ss' or Unix seconds.")
		.arg(fieldName, raw);
	return false;
}

QString normalizeCustomUnit(const QString &raw) {
	const QString lower = raw.trimmed().toLower();
	if (lower == QStringLiteral("day") || lower == QStringLiteral("days") || lower == QStringLiteral("daily")) {
		return QStringLiteral("Day");
	}
	if (lower == QStringLiteral("week") || lower == QStringLiteral("weeks") || lower == QStringLiteral("weekly")) {
		return QStringLiteral("Week");
	}
	if (lower == QStringLiteral("month") || lower == QStringLiteral("months") || lower == QStringLiteral("monthly")) {
		return QStringLiteral("Month");
	}
	return raw.trimmed();
}

MeetingRepeatType normalizeRepeatType(const QString &raw) {
	const QString lower = raw.trimmed().toLower();
	if (lower.isEmpty() || lower == QStringLiteral("none") || lower == QStringLiteral("once")) {
		return MeetingRepeatType::None;
	}
	if (lower == QStringLiteral("daily") || lower == QStringLiteral("everyday")) {
		return MeetingRepeatType::Daily;
	}
	if (lower == QStringLiteral("weekly")) {
		return MeetingRepeatType::Weekly;
	}
	if (lower == QStringLiteral("weekday") || lower == QStringLiteral("weekdays") || lower == QStringLiteral("workday")) {
		return MeetingRepeatType::WeekDay;
	}
	if (lower == QStringLiteral("monthly")) {
		return MeetingRepeatType::Monthly;
	}
	if (lower == QStringLiteral("custom")) {
		return MeetingRepeatType::Custom;
	}
	return MeetingRepeatType::Unknown;
}

bool parseRepeatRuleFromArgs(
		const QJsonObject &args,
		qint64 scheduledTimeSeconds,
		const QString &timeZoneId,
		MeetingRepeatRule &outRule,
		bool &outProvided,
		QString &outError) {
	outRule = MeetingRepeatRule{};
	outProvided = false;

	const bool hasType = args.contains(QStringLiteral("repeat_type")) && !args.value(QStringLiteral("repeat_type")).isNull();
	const bool hasUnit = args.contains(QStringLiteral("repeat_unit")) && !args.value(QStringLiteral("repeat_unit")).isNull();
	const bool hasInterval = args.contains(QStringLiteral("repeat_interval")) && !args.value(QStringLiteral("repeat_interval")).isNull();
	const bool hasDays = args.contains(QStringLiteral("repeat_days_of_week")) && args.value(QStringLiteral("repeat_days_of_week")).isArray() &&
		!args.value(QStringLiteral("repeat_days_of_week")).toArray().isEmpty();
	const bool hasEnd = args.contains(QStringLiteral("repeat_end_time")) && !args.value(QStringLiteral("repeat_end_time")).isNull() &&
		!args.value(QStringLiteral("repeat_end_time")).toString().trimmed().isEmpty();
	const bool hasTimes = args.contains(QStringLiteral("repeat_times")) && !args.value(QStringLiteral("repeat_times")).isNull() &&
		args.value(QStringLiteral("repeat_times")).toInt(0) > 0;

	if (!hasType && !hasUnit && !hasInterval && !hasDays && !hasEnd && !hasTimes) {
		return true;
	}
	outProvided = true;

	MeetingRepeatType type = hasType
		? normalizeRepeatType(args.value(QStringLiteral("repeat_type")).toString())
		: MeetingRepeatType::None;
	if (type == MeetingRepeatType::Unknown) {
		outError = QStringLiteral("Unsupported repeat_type '%1'. Use None, Daily, Weekly, WeekDay, Monthly, or Custom.")
			.arg(args.value(QStringLiteral("repeat_type")).toString());
		return false;
	}

	const int requestedInterval = args.value(QStringLiteral("repeat_interval")).toInt(0);
	if (type != MeetingRepeatType::Custom &&
		(hasUnit || hasDays || hasTimes || requestedInterval > 1)) {
		if (type == MeetingRepeatType::Daily && !hasUnit) {
			outRule.unitType = QStringLiteral("Day");
		} else if (type == MeetingRepeatType::Monthly && !hasUnit) {
			outRule.unitType = QStringLiteral("Month");
		} else if (!hasUnit) {
			outRule.unitType = QStringLiteral("Week");
		}
		type = MeetingRepeatType::Custom;
	}

	outRule.type = type;
	outRule.rawType = meetingRepeatTypeToWire(type);

	if (hasEnd) {
		qint64 endSec = 0;
		if (!parseFlexibleTimestamp(
				args.value(QStringLiteral("repeat_end_time")),
				timeZoneId,
				scheduledTimeSeconds,
				endSec,
				outError,
				QStringLiteral("repeat_end_time"))) {
			return false;
		}
		outRule.endDateSeconds = endSec;
	}

	if (type == MeetingRepeatType::Custom) {
		if (outRule.unitType.isEmpty()) {
			const QString rawUnit = args.value(QStringLiteral("repeat_unit")).toString();
			outRule.unitType = rawUnit.trimmed().isEmpty()
				? (hasDays ? QStringLiteral("Week") : QStringLiteral("Day"))
				: normalizeCustomUnit(rawUnit);
		}
		outRule.interval = hasInterval ? args.value(QStringLiteral("repeat_interval")).toInt(0) : 1;
		outRule.repeatTimes = hasTimes ? args.value(QStringLiteral("repeat_times")).toInt(0) : 0;

		if (hasDays) {
			const QJsonArray arr = args.value(QStringLiteral("repeat_days_of_week")).toArray();
			for (const auto &v : arr) {
				if (!v.isDouble()) {
					outError = QStringLiteral("repeat_days_of_week must contain integers 0..6 (0=Sunday..6=Saturday).");
					return false;
				}
				int day = v.toInt(-1);
				if (day == 7) {
					day = 0;
				}
				if (day < 0 || day > 6) {
					outError = QStringLiteral("repeat_days_of_week value %1 is out of range (0..6).").arg(day);
					return false;
				}
				if (std::find(outRule.daysOfWeek.begin(), outRule.daysOfWeek.end(), day) == outRule.daysOfWeek.end()) {
					outRule.daysOfWeek.push_back(day);
				}
			}
			std::sort(outRule.daysOfWeek.begin(), outRule.daysOfWeek.end());
		}
	}

	return isMeetingRepeatRuleSupportedForWrite(outRule, &outError, true);
}

QString formatRepeatRuleSummary(const MeetingRepeatRule &rule, const QString &timeZoneId) {
	QString base;
	switch (rule.type) {
	case MeetingRepeatType::None:
		return QCoreApplication::translate("MeetingUI", "Does Not Repeat");
	case MeetingRepeatType::Daily:
		base = QCoreApplication::translate("MeetingUI", "Daily");
		break;
	case MeetingRepeatType::Weekly:
		base = QCoreApplication::translate("MeetingUI", "Weekly");
		break;
	case MeetingRepeatType::WeekDay:
		base = QCoreApplication::translate("MeetingUI", "Weekdays (Mon-Fri)");
		break;
	case MeetingRepeatType::Monthly:
		base = QCoreApplication::translate("MeetingUI", "Monthly");
		break;
	case MeetingRepeatType::Custom: {
		QString unitLabel = rule.unitType;
		if (rule.unitType == QStringLiteral("Day")) {
			unitLabel = QCoreApplication::translate("MeetingUI", "day(s)");
		} else if (rule.unitType == QStringLiteral("Week")) {
			unitLabel = QCoreApplication::translate("MeetingUI", "week(s)");
		} else if (rule.unitType == QStringLiteral("Month")) {
			unitLabel = QCoreApplication::translate("MeetingUI", "month(s)");
		}
		base = QCoreApplication::translate("MeetingUI", "Every %1 %2")
			.arg(std::max(1, rule.interval))
			.arg(unitLabel);
		if (!rule.daysOfWeek.empty()) {
			static const QStringList kDayNames = {
				QCoreApplication::translate("MeetingUI", "Sun"),
				QCoreApplication::translate("MeetingUI", "Mon"),
				QCoreApplication::translate("MeetingUI", "Tue"),
				QCoreApplication::translate("MeetingUI", "Wed"),
				QCoreApplication::translate("MeetingUI", "Thu"),
				QCoreApplication::translate("MeetingUI", "Fri"),
				QCoreApplication::translate("MeetingUI", "Sat"),
			};
			QStringList dayLabels;
			for (const int d : rule.daysOfWeek) {
				if (d >= 0 && d < kDayNames.size()) {
					dayLabels.push_back(kDayNames.at(d));
				}
			}
			if (!dayLabels.isEmpty()) {
				base += QStringLiteral(" (%1)").arg(dayLabels.join(QStringLiteral(", ")));
			}
		}
		if (rule.repeatTimes > 0) {
			base += QCoreApplication::translate("MeetingUI", ", %1 occurrence(s)").arg(rule.repeatTimes);
		}
		break;
	}
	case MeetingRepeatType::Unknown:
	default:
		base = rule.rawType.isEmpty() ? QStringLiteral("Unknown") : rule.rawType;
		break;
	}

	if (rule.endDateSeconds > 0) {
		const QTimeZone tz = resolveValidTimeZone(timeZoneId);
		const QDateTime endDt = QDateTime::fromSecsSinceEpoch(rule.endDateSeconds, tz);
		if (endDt.isValid()) {
			base += QCoreApplication::translate("MeetingUI", ", until %1")
				.arg(endDt.date().toString(QStringLiteral("yyyy-MM-dd")));
		}
	}
	return base;
}

bool parseBookingRequestFromArgs(
		const QJsonObject &args,
		MeetingBookingRequest &outReq,
		QString &outError,
		const MediaPreferences *defaultMediaPrefs = nullptr) {
	outReq = MeetingBookingRequest{};
	outReq.allowCustomRepeatRule = true;
	outReq.title = args.value(QStringLiteral("title")).toString().trimmed();
	if (outReq.title.isEmpty()) {
		outError = QStringLiteral("Missing or empty 'title'.");
		return false;
	}
	if (outReq.title.size() > 128) {
		outReq.title = outReq.title.left(128);
	}

	QString canonicalTz;
	resolveValidTimeZone(args.value(QStringLiteral("time_zone")).toString(), &canonicalTz);
	outReq.timeZone = canonicalTz;

	if (!args.contains(QStringLiteral("start_time"))) {
		outError = QStringLiteral("Missing required parameter 'start_time'.");
		return false;
	}
	if (!parseFlexibleTimestamp(
			args.value(QStringLiteral("start_time")),
			outReq.timeZone,
			0,
			outReq.scheduledTimeSeconds,
			outError,
			QStringLiteral("start_time"))) {
		return false;
	}

	const int durationMinutes = args.contains(QStringLiteral("duration_minutes"))
		? args.value(QStringLiteral("duration_minutes")).toInt(60)
		: 60;
	if (durationMinutes < 1 || durationMinutes > 7 * 24 * 60) {
		outError = QStringLiteral("duration_minutes must be between 1 and 10080.");
		return false;
	}
	outReq.meetingDurationSeconds = static_cast<qint64>(durationMinutes) * 60;
	outReq.password = args.value(QStringLiteral("password")).toString().trimmed();

	outReq.settings.canParticipantsEnableCamera = true;
	outReq.settings.canParticipantsUnmuteMicrophone = true;
	outReq.settings.canParticipantsShareScreen = true;
	const bool defaultDisableCamera = defaultMediaPrefs ? !defaultMediaPrefs->enableVideo : true;
	const bool defaultDisableMicrophone = defaultMediaPrefs ? !defaultMediaPrefs->enableMicrophone : false;
	outReq.settings.disableCameraOnJoin = args.value(QStringLiteral("disable_camera_on_join")).toBool(defaultDisableCamera);
	outReq.settings.disableMicrophoneOnJoin = args.value(QStringLiteral("disable_microphone_on_join")).toBool(defaultDisableMicrophone);
	outReq.settings.canParticipantJoinMeetingEarly = args.value(QStringLiteral("allow_early_join")).toBool(true);

	if (args.contains(QStringLiteral("invitee_user_ids")) && !args.value(QStringLiteral("invitee_user_ids")).isNull()) {
		if (!args.value(QStringLiteral("invitee_user_ids")).isArray()) {
			outError = QStringLiteral("invitee_user_ids must be an array of userId strings.");
			return false;
		}
		QSet<QString> seenInvitees;
		for (const auto &v : args.value(QStringLiteral("invitee_user_ids")).toArray()) {
			const QString uid = v.toString().trimmed();
			if (!uid.isEmpty() && !seenInvitees.contains(uid)) {
				seenInvitees.insert(uid);
				outReq.inviteeUserIds.push_back(uid);
			}
		}
	}

	bool repeatProvided = false;
	if (!parseRepeatRuleFromArgs(
			args,
			outReq.scheduledTimeSeconds,
			outReq.timeZone,
			outReq.repeatRule,
			repeatProvided,
			outError)) {
		return false;
	}

	return validateMeetingBookingRequest(outReq, &outError);
}

bool parseUpdateRequestFromArgs(
		const QJsonObject &args,
		MeetingUpdateRequest &outReq,
		QString &outError) {
	outReq = MeetingUpdateRequest{};
	outReq.allowCustomRepeatRule = true;
	outReq.meetingId = args.value(QStringLiteral("meeting_id")).toString().trimmed();
	if (outReq.meetingId.isEmpty()) {
		outError = QStringLiteral("Missing required parameter 'meeting_id'.");
		return false;
	}

	QString canonicalTz = defaultSystemTimeZoneId();
	if (args.contains(QStringLiteral("time_zone")) && !args.value(QStringLiteral("time_zone")).toString().trimmed().isEmpty()) {
		resolveValidTimeZone(args.value(QStringLiteral("time_zone")).toString(), &canonicalTz);
		outReq.timeZone = canonicalTz;
	}

	if (args.contains(QStringLiteral("title")) && !args.value(QStringLiteral("title")).isNull()) {
		outReq.title = args.value(QStringLiteral("title")).toString().trimmed().left(128);
	}

	qint64 refStartSec = 0;
	if (args.contains(QStringLiteral("start_time")) && !args.value(QStringLiteral("start_time")).isNull()) {
		if (!parseFlexibleTimestamp(
				args.value(QStringLiteral("start_time")),
				canonicalTz,
				0,
				refStartSec,
				outError,
				QStringLiteral("start_time"))) {
			return false;
		}
		outReq.scheduledTimeSeconds = refStartSec;
	}

	if (args.contains(QStringLiteral("duration_minutes")) && !args.value(QStringLiteral("duration_minutes")).isNull()) {
		const int mins = args.value(QStringLiteral("duration_minutes")).toInt(0);
		if (mins < 1 || mins > 7 * 24 * 60) {
			outError = QStringLiteral("duration_minutes must be between 1 and 10080.");
			return false;
		}
		outReq.meetingDurationSeconds = static_cast<qint64>(mins) * 60;
	}

	if (args.contains(QStringLiteral("password")) && !args.value(QStringLiteral("password")).isNull()) {
		outReq.password = args.value(QStringLiteral("password")).toString().trimmed();
	}
	if (args.contains(QStringLiteral("disable_camera_on_join")) && args.value(QStringLiteral("disable_camera_on_join")).isBool()) {
		outReq.disableCameraOnJoin = args.value(QStringLiteral("disable_camera_on_join")).toBool();
	}
	if (args.contains(QStringLiteral("disable_microphone_on_join")) && args.value(QStringLiteral("disable_microphone_on_join")).isBool()) {
		outReq.disableMicrophoneOnJoin = args.value(QStringLiteral("disable_microphone_on_join")).toBool();
	}
	if (args.contains(QStringLiteral("allow_early_join")) && args.value(QStringLiteral("allow_early_join")).isBool()) {
		outReq.canParticipantJoinMeetingEarly = args.value(QStringLiteral("allow_early_join")).toBool();
	}
	if (args.contains(QStringLiteral("invitee_user_ids")) && !args.value(QStringLiteral("invitee_user_ids")).isNull()) {
		if (!args.value(QStringLiteral("invitee_user_ids")).isArray()) {
			outError = QStringLiteral("invitee_user_ids must be an array of userId strings.");
			return false;
		}
		std::vector<QString> ids;
		QSet<QString> seenInvitees;
		for (const auto &v : args.value(QStringLiteral("invitee_user_ids")).toArray()) {
			const QString uid = v.toString().trimmed();
			if (!uid.isEmpty() && !seenInvitees.contains(uid)) {
				seenInvitees.insert(uid);
				ids.push_back(uid);
			}
		}
		outReq.inviteeUserIds = std::move(ids);
	}

	MeetingRepeatRule rule;
	bool repeatProvided = false;
	if (!parseRepeatRuleFromArgs(args, refStartSec, canonicalTz, rule, repeatProvided, outError)) {
		return false;
	}
	if (repeatProvided) {
		outReq.repeatRule = rule;
	}

	return validateMeetingUpdateRequest(outReq, &outError);
}

QJsonObject serializeMeetingRecordForTool(const MeetingRecord &m, bool includeSettings = false) {
	const QTimeZone tz = resolveValidTimeZone(m.timeZone);
	const qint64 effectiveSec = m.scheduledTimeSeconds > 0 ? m.scheduledTimeSeconds : m.startTimeSeconds;
	const QDateTime dt = effectiveSec > 0 ? QDateTime::fromSecsSinceEpoch(effectiveSec, tz) : QDateTime{};

	QJsonObject row;
	row.insert(QStringLiteral("meetingId"), m.meetingId);
	row.insert(QStringLiteral("title"), m.title);
	row.insert(QStringLiteral("status"), meetingStatusToWire(m.status));
	row.insert(QStringLiteral("scheduledTimeUnix"), static_cast<qint64>(effectiveSec));
	if (dt.isValid()) {
		row.insert(QStringLiteral("scheduledTimeLocal"), dt.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
	}
	row.insert(QStringLiteral("durationMinutes"), static_cast<int>(m.meetingDurationSeconds / 60));
	row.insert(QStringLiteral("timeZone"), m.timeZone.isEmpty() ? defaultSystemTimeZoneId() : m.timeZone);
	row.insert(QStringLiteral("repeatType"), meetingRepeatTypeToWire(m.repeatRule.type));
	row.insert(QStringLiteral("repeatSummary"), formatRepeatRuleSummary(m.repeatRule, m.timeZone));
	row.insert(QStringLiteral("creator"), m.creatorNickname.trimmed().isEmpty() ? m.creatorUserId : m.creatorNickname.trimmed());
	if (!m.inviteeUserIds.empty()) {
		QJsonArray invitees;
		for (const auto &uid : m.inviteeUserIds) {
			if (!uid.trimmed().isEmpty()) {
				invitees.append(uid.trimmed());
			}
		}
		if (!invitees.isEmpty()) {
			row.insert(QStringLiteral("inviteeUserIds"), invitees);
		}
	}
	if (includeSettings) {
		QJsonObject settingsObj;
		settingsObj.insert(QStringLiteral("disableCameraOnJoin"), m.settings.disableCameraOnJoin);
		settingsObj.insert(QStringLiteral("disableMicrophoneOnJoin"), m.settings.disableMicrophoneOnJoin);
		settingsObj.insert(QStringLiteral("allowEarlyJoin"), m.settings.canParticipantJoinMeetingEarly);
		settingsObj.insert(QStringLiteral("lockMeeting"), m.settings.lockMeeting);
		settingsObj.insert(QStringLiteral("canParticipantsEnableCamera"), m.settings.canParticipantsEnableCamera);
		settingsObj.insert(QStringLiteral("canParticipantsUnmuteMicrophone"), m.settings.canParticipantsUnmuteMicrophone);
		settingsObj.insert(QStringLiteral("canParticipantsShareScreen"), m.settings.canParticipantsShareScreen);
		row.insert(QStringLiteral("settings"), settingsObj);
	}
	return row;
}

QJsonObject buildScheduleToolProperties() {
	QJsonObject bookProps;
	bookProps.insert(QStringLiteral("title"), makeProp(
		QStringLiteral("string"),
		QStringLiteral("Meeting title (1-128 characters).")));
	bookProps.insert(QStringLiteral("start_time"), makeProp(
		QStringLiteral("string"),
		QStringLiteral("Scheduled start time as ISO-8601 local/offset string (e.g. '2026-11-17T15:00:00') or Unix seconds.")));
	bookProps.insert(QStringLiteral("duration_minutes"), makeProp(
		QStringLiteral("integer"),
		QStringLiteral("Meeting duration in minutes (1-10080, default 60).")));
	bookProps.insert(QStringLiteral("time_zone"), makeProp(
		QStringLiteral("string"),
		QStringLiteral("Optional IANA time zone ID (e.g. 'Asia/Shanghai'). Defaults to local system time zone.")));
	bookProps.insert(QStringLiteral("password"), makeProp(
		QStringLiteral("string"),
		QStringLiteral("Optional meeting password. Omit or leave empty unless the user explicitly asks to set a password.")));
	bookProps.insert(QStringLiteral("repeat_type"), makeProp(
		QStringLiteral("string"),
		QStringLiteral("Recurrence type: 'None' (default), 'Daily', 'Weekly', 'WeekDay', 'Monthly', or 'Custom'.")));
	bookProps.insert(QStringLiteral("repeat_unit"), makeProp(
		QStringLiteral("string"),
		QStringLiteral("When repeat_type is 'Custom': 'Day', 'Week', or 'Month'.")));
	bookProps.insert(QStringLiteral("repeat_interval"), makeProp(
		QStringLiteral("integer"),
		QStringLiteral("When repeat_type is 'Custom': positive interval count (1-365, e.g. 2 for every 2 weeks).")));
	bookProps.insert(QStringLiteral("repeat_days_of_week"), makeArrayProp(
		QStringLiteral("integer"),
		QStringLiteral("Optional when repeat_type is 'Custom' and repeat_unit is 'Week': days of week (0=Sunday, 1=Monday, ..., 6=Saturday).")));
	bookProps.insert(QStringLiteral("repeat_end_time"), makeProp(
		QStringLiteral("string"),
		QStringLiteral("Optional recurrence end date/time ('YYYY-MM-DD', ISO-8601 datetime, or Unix seconds).")));
	bookProps.insert(QStringLiteral("repeat_times"), makeProp(
		QStringLiteral("integer"),
		QStringLiteral("Optional when repeat_type is 'Custom': total occurrence count (1-1000).")));
	bookProps.insert(QStringLiteral("disable_camera_on_join"), makeProp(
		QStringLiteral("boolean"),
		QStringLiteral("Optional. Turn off participant cameras when joining (default true).")));
	bookProps.insert(QStringLiteral("disable_microphone_on_join"), makeProp(
		QStringLiteral("boolean"),
		QStringLiteral("Optional. Mute participant microphones when joining (default false).")));
	bookProps.insert(QStringLiteral("allow_early_join"), makeProp(
		QStringLiteral("boolean"),
		QStringLiteral("Optional. Allow participants to join before the host (default true).")));
	bookProps.insert(QStringLiteral("invitee_user_ids"), makeArrayProp(
		QStringLiteral("string"),
		QStringLiteral("Optional list of contact userIds (from search_contacts) to invite to this meeting.")));
	return bookProps;
}

} // namespace

QString MeetingToolRegistry::formatScheduledMeetingCompletionSummary(
		const QJsonObject &args,
		const QString &meetingId) {
	MeetingBookingRequest req;
	QString err;
	if (!parseBookingRequestFromArgs(args, req, err)) {
		return QCoreApplication::translate(
			"MeetingUI",
			"Meeting scheduled successfully (Meeting ID: %1). The schedule list has been refreshed.")
			.arg(meetingId);
	}

	const QTimeZone tz = resolveValidTimeZone(req.timeZone);
	const QDateTime dt = QDateTime::fromSecsSinceEpoch(req.scheduledTimeSeconds, tz);
	const int durationMins = static_cast<int>(req.meetingDurationSeconds / 60);
	const QString repeatSummary = formatRepeatRuleSummary(req.repeatRule, req.timeZone);

	if (req.password.isEmpty()) {
		return QCoreApplication::translate(
			"MeetingUI",
			"Meeting \"%1\" (ID: %2) has been scheduled for %3 (%4 min, %5). "
			"Created without a password by default since none was specified, and the schedule list has been refreshed.")
			.arg(req.title,
				meetingId,
				dt.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
				QString::number(durationMins),
				repeatSummary);
	}

	return QCoreApplication::translate(
		"MeetingUI",
		"Meeting \"%1\" (ID: %2) has been scheduled for %3 (%4 min, %5, password protected), and the schedule list has been refreshed.")
		.arg(req.title,
			meetingId,
			dt.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
			QString::number(durationMins),
			repeatSummary);
}

void MeetingToolRegistry::registerCatalogTools() {
	// list_scheduled_meetings
	{
		RegisteredTool t;
		QJsonObject listSchedProps;
		listSchedProps.insert(QStringLiteral("include_history"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Optional. True to also include completed meetings from history (default false).")));
		listSchedProps.insert(QStringLiteral("limit"), makeProp(
			QStringLiteral("integer"),
			QStringLiteral("Maximum number of meetings to return (1-50, default 20).")));
		t.definition = {
			QStringLiteral("list_scheduled_meetings"),
			QStringLiteral("List the user's scheduled, in-progress, or completed meetings from the schedule catalog."),
			makeObjectSchema(listSchedProps),
			AgentToolRisk::ReadOnly,
			AgentToolScope::AccountCatalog,
		};
		t.executeReadOnly = [](const QJsonObject &args, const AgentToolViewContext &ctx) {
			const QString name = QStringLiteral("list_scheduled_meetings");
			if (!ctx.catalog) {
				return makeErrorResult(name, QStringLiteral("Meeting catalog is unavailable."));
			}
			const bool includeHistory = args.value(QStringLiteral("include_history")).toBool(false);
			const int limit = qBound(1, args.value(QStringLiteral("limit")).toInt(20), 50);

			const auto &upcoming = ctx.catalog->upcomingState();
			const auto &history = ctx.catalog->historyState();
			if (upcoming.state == MeetingCatalogLoadState::Error && !upcoming.hasSnapshot) {
				return makeErrorResult(name, upcoming.error.message.isEmpty()
					? QStringLiteral("Failed to load scheduled meetings.")
					: upcoming.error.message);
			}

			QJsonArray items;
			QSet<QString> seenIds;
			int totalMatching = 0;

			const auto appendList = [&](const MeetingList &list) {
				for (const auto &m : list) {
					if (m.meetingId.isEmpty() || seenIds.contains(m.meetingId)) {
						continue;
					}
					seenIds.insert(m.meetingId);
					++totalMatching;
					if (items.size() < limit) {
						items.append(serializeMeetingRecordForTool(m));
					}
				}
			};

			appendList(upcoming.meetings);
			if (includeHistory) {
				appendList(history.meetings);
			}

			QJsonObject payload;
			payload.insert(QStringLiteral("totalCount"), totalMatching);
			payload.insert(QStringLiteral("truncated"), totalMatching > items.size());
			payload.insert(QStringLiteral("meetings"), items);
			return makeOkResult(name, payload);
		};
		registerTool(std::move(t));
	}

	// get_scheduled_meeting_detail
	{
		RegisteredTool t;
		QJsonObject detailProps;
		detailProps.insert(QStringLiteral("meeting_id"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Target scheduled meeting ID.")));
		t.definition = {
			QStringLiteral("get_scheduled_meeting_detail"),
			QStringLiteral("Get detailed metadata, recurrence, and join-permission settings for a scheduled meeting (redacts password value)."),
			makeObjectSchema(detailProps, {QStringLiteral("meeting_id")}),
			AgentToolRisk::ReadOnly,
			AgentToolScope::AccountCatalog,
		};
		t.executeReadOnly = [](const QJsonObject &args, const AgentToolViewContext &ctx) {
			const QString name = QStringLiteral("get_scheduled_meeting_detail");
			if (!ctx.catalog) {
				return makeErrorResult(name, QStringLiteral("Meeting catalog is unavailable."));
			}
			const QString meetingId = args.value(QStringLiteral("meeting_id")).toString().trimmed();
			if (meetingId.isEmpty()) {
				return makeErrorResult(name, QStringLiteral("Missing required parameter 'meeting_id'."));
			}
			const auto &detailState = ctx.catalog->detailState();
			if (detailState.detail.has_value() && detailState.detail->record.meetingId == meetingId) {
				QJsonObject obj = serializeMeetingRecordForTool(detailState.detail->record, true);
				obj.insert(QStringLiteral("passwordProtected"), !detailState.detail->password.isEmpty());
				return makeOkResult(name, obj);
			}
			for (const auto &m : ctx.catalog->upcomingState().meetings) {
				if (m.meetingId == meetingId) {
					return makeOkResult(name, serializeMeetingRecordForTool(m, true));
				}
			}
			for (const auto &m : ctx.catalog->historyState().meetings) {
				if (m.meetingId == meetingId) {
					return makeOkResult(name, serializeMeetingRecordForTool(m, true));
				}
			}
			return makeErrorResult(name, QStringLiteral("Scheduled meeting '%1' not found in catalog.").arg(meetingId));
		};
		registerTool(std::move(t));
	}

	// schedule_meeting
	{
		RegisteredTool t;
		t.definition = {
			QStringLiteral("schedule_meeting"),
			QStringLiteral("Schedule (book) a new one-time, standard recurring, or custom recurring meeting (requires user confirmation). Defaults to no password unless specified."),
			makeObjectSchema(buildScheduleToolProperties(), {QStringLiteral("title"), QStringLiteral("start_time")}),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::AccountCatalog,
			AgentConcurrencyGroup::CatalogWrite,
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &ctx,
				PreparedPendingAction &out, QString &outError) {
			MeetingBookingRequest req;
			const MediaPreferences *mediaPrefs = ctx.sessionManager ? &ctx.sessionManager->mediaPreferences() : nullptr;
			if (!parseBookingRequestFromArgs(args, req, outError, mediaPrefs)) {
				return false;
			}
			const QTimeZone tz = resolveValidTimeZone(req.timeZone);
			const QDateTime startDt = QDateTime::fromSecsSinceEpoch(req.scheduledTimeSeconds, tz);
			const QString timeStr = startDt.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
			const int mins = static_cast<int>(req.meetingDurationSeconds / 60);
			const QString repeatStr = formatRepeatRuleSummary(req.repeatRule, req.timeZone);
			const QString pwdStr = req.password.isEmpty()
				? QCoreApplication::translate("MeetingUI", "None (Default)")
				: QCoreApplication::translate("MeetingUI", "Password Protected");

			out.title = QCoreApplication::translate("MeetingUI", "Schedule Meeting");
			out.summary = QCoreApplication::translate(
				"MeetingUI",
				"Title: %1\nStart: %2 (%3, %4 min)\nRecurrence: %5\nPassword: %6")
				.arg(req.title, timeStr, req.timeZone, QString::number(mins), repeatStr, pwdStr);
			if (!req.inviteeUserIds.empty()) {
				QStringList names;
				for (const auto &uid : req.inviteeUserIds) {
					names.push_back(ctx.contacts ? ctx.contacts->displayNameForUser(uid) : uid);
				}
				out.summary.append(QLatin1Char('\n'));
				out.summary.append(QCoreApplication::translate("MeetingUI", "Invitees: %1")
					.arg(names.join(QStringLiteral(", "))));
			}
			out.normalizedArgs = args;
			out.normalizedArgs.insert(QStringLiteral("time_zone"), req.timeZone);
			return true;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("schedule_meeting");
			if (!ctx.catalog) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Meeting catalog controller is unavailable.")));
			}
			MeetingBookingRequest req;
			QString err;
			const MediaPreferences *mediaPrefs = ctx.sessionManager ? &ctx.sessionManager->mediaPreferences() : nullptr;
			if (!parseBookingRequestFromArgs(args, req, err, mediaPrefs)) {
				return makeSimpleOutcome(makeErrorResult(name, err));
			}
			if (!ctx.catalog->bookMeeting(req)) {
				const QString msg = ctx.catalog->lastWriteResult().error.message;
				return makeSimpleOutcome(makeErrorResult(name, msg.isEmpty() ? QStringLiteral("Failed to book meeting.") : msg));
			}
			const auto &writeRes = ctx.catalog->lastWriteResult();
			QJsonObject payload;
			payload.insert(QStringLiteral("inFlight"), writeRes.inFlight);
			if (!writeRes.meetingId.isEmpty()) {
				payload.insert(QStringLiteral("meetingId"), writeRes.meetingId);
			}
			payload.insert(QStringLiteral("passwordProtected"), !req.password.isEmpty());
			ToolExecutionOutcome outcome;
			outcome.toolResult = makeOkResult(name, payload);
			outcome.asyncInFlight = writeRes.inFlight;
			return outcome;
		};
		registerTool(std::move(t));
	}

	// update_scheduled_meeting
	{
		RegisteredTool t;
		QJsonObject updateProps = buildScheduleToolProperties();
		updateProps.insert(QStringLiteral("meeting_id"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Target scheduled meeting ID to update.")));
		t.definition = {
			QStringLiteral("update_scheduled_meeting"),
			QStringLiteral("Update an existing scheduled meeting's title, start time, duration, recurrence, or settings (requires user confirmation)."),
			makeObjectSchema(updateProps, {QStringLiteral("meeting_id")}),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::AccountCatalog,
			AgentConcurrencyGroup::CatalogWrite,
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &ctx,
				PreparedPendingAction &out, QString &outError) {
			MeetingUpdateRequest req;
			if (!parseUpdateRequestFromArgs(args, req, outError)) {
				return false;
			}
			QStringList changes;
			if (req.title) {
				changes.push_back(QCoreApplication::translate("MeetingUI", "Title -> %1").arg(*req.title));
			}
			if (req.scheduledTimeSeconds) {
				const QString tzId = req.timeZone.value_or(defaultSystemTimeZoneId());
				const QDateTime dt = QDateTime::fromSecsSinceEpoch(*req.scheduledTimeSeconds, resolveValidTimeZone(tzId));
				changes.push_back(QCoreApplication::translate("MeetingUI", "Start -> %1 (%2)")
					.arg(dt.toString(QStringLiteral("yyyy-MM-dd HH:mm")), tzId));
			}
			if (req.meetingDurationSeconds) {
				changes.push_back(QCoreApplication::translate("MeetingUI", "Duration -> %1 min")
					.arg(*req.meetingDurationSeconds / 60));
			}
			if (req.repeatRule) {
				changes.push_back(QCoreApplication::translate("MeetingUI", "Recurrence -> %1")
					.arg(formatRepeatRuleSummary(*req.repeatRule, req.timeZone.value_or(defaultSystemTimeZoneId()))));
			}
			if (req.password) {
				changes.push_back(req.password->isEmpty()
					? QCoreApplication::translate("MeetingUI", "Password -> Cleared")
					: QCoreApplication::translate("MeetingUI", "Password -> Updated"));
			}
			if (req.disableMicrophoneOnJoin) {
				changes.push_back(*req.disableMicrophoneOnJoin
					? QCoreApplication::translate("MeetingUI", "Mute Participants on Join -> Enabled")
					: QCoreApplication::translate("MeetingUI", "Mute Participants on Join -> Disabled"));
			}
			if (req.disableCameraOnJoin) {
				changes.push_back(*req.disableCameraOnJoin
					? QCoreApplication::translate("MeetingUI", "Turn Off Camera on Join -> Enabled")
					: QCoreApplication::translate("MeetingUI", "Turn Off Camera on Join -> Disabled"));
			}
			if (req.canParticipantJoinMeetingEarly) {
				changes.push_back(*req.canParticipantJoinMeetingEarly
					? QCoreApplication::translate("MeetingUI", "Allow Early Join -> Enabled")
					: QCoreApplication::translate("MeetingUI", "Allow Early Join -> Disabled"));
			}
			if (req.inviteeUserIds) {
				if (req.inviteeUserIds->empty()) {
					changes.push_back(QCoreApplication::translate("MeetingUI", "Invitees -> Cleared"));
				} else {
					QStringList names;
					for (const auto &uid : *req.inviteeUserIds) {
						names.push_back(ctx.contacts ? ctx.contacts->displayNameForUser(uid) : uid);
					}
					changes.push_back(QCoreApplication::translate("MeetingUI", "Invitees -> %1")
						.arg(names.join(QStringLiteral(", "))));
				}
			}
			out.title = QCoreApplication::translate("MeetingUI", "Update Scheduled Meeting");
			out.summary = QCoreApplication::translate("MeetingUI", "Meeting ID: %1\n%2")
				.arg(req.meetingId, changes.join(QStringLiteral("\n")));
			out.normalizedArgs = args;
			if (req.timeZone) {
				out.normalizedArgs.insert(QStringLiteral("time_zone"), *req.timeZone);
			}
			return true;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("update_scheduled_meeting");
			if (!ctx.catalog) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Meeting catalog controller is unavailable.")));
			}
			MeetingUpdateRequest req;
			QString err;
			if (!parseUpdateRequestFromArgs(args, req, err)) {
				return makeSimpleOutcome(makeErrorResult(name, err));
			}
			if (!ctx.catalog->updateMeeting(req)) {
				const QString msg = ctx.catalog->lastWriteResult().error.message;
				return makeSimpleOutcome(makeErrorResult(name, msg.isEmpty() ? QStringLiteral("Failed to update meeting.") : msg));
			}
			const auto &writeRes = ctx.catalog->lastWriteResult();
			QJsonObject payload;
			payload.insert(QStringLiteral("meetingId"), req.meetingId);
			payload.insert(QStringLiteral("inFlight"), writeRes.inFlight);
			ToolExecutionOutcome outcome;
			outcome.toolResult = makeOkResult(name, payload);
			outcome.asyncInFlight = writeRes.inFlight;
			return outcome;
		};
		registerTool(std::move(t));
	}

	// cancel_scheduled_meeting
	{
		RegisteredTool t;
		QJsonObject cancelProps;
		cancelProps.insert(QStringLiteral("meeting_id"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Target scheduled meeting ID to cancel.")));
		cancelProps.insert(QStringLiteral("title"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Optional meeting title to display on the confirmation card.")));
		t.definition = {
			QStringLiteral("cancel_scheduled_meeting"),
			QStringLiteral("Cancel a scheduled meeting by meeting_id (requires user confirmation)."),
			makeObjectSchema(cancelProps, {QStringLiteral("meeting_id")}),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::AccountCatalog,
			AgentConcurrencyGroup::CatalogWrite,
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &ctx,
				PreparedPendingAction &out, QString &outError) {
			const QString meetingId = args.value(QStringLiteral("meeting_id")).toString().trimmed();
			if (meetingId.isEmpty()) {
				outError = QStringLiteral("Missing required parameter 'meeting_id'.");
				return false;
			}
			QString knownTitle = args.value(QStringLiteral("title")).toString().trimmed();
			if (knownTitle.isEmpty() && ctx.catalog) {
				for (const auto &m : ctx.catalog->upcomingState().meetings) {
					if (m.meetingId == meetingId) {
						knownTitle = m.title.trimmed();
						break;
					}
				}
			}
			out.title = QCoreApplication::translate("MeetingUI", "Cancel Scheduled Meeting");
			out.summary = knownTitle.isEmpty()
				? QCoreApplication::translate("MeetingUI", "Cancel scheduled meeting (ID: %1).").arg(meetingId)
				: QCoreApplication::translate("MeetingUI", "Cancel \"%1\" (ID: %2).").arg(knownTitle, meetingId);
			out.normalizedArgs = args;
			out.normalizedArgs.insert(QStringLiteral("meeting_id"), meetingId);
			return true;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("cancel_scheduled_meeting");
			if (!ctx.catalog) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Meeting catalog controller is unavailable.")));
			}
			const QString meetingId = args.value(QStringLiteral("meeting_id")).toString().trimmed();
			if (meetingId.isEmpty()) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Missing required parameter 'meeting_id'.")));
			}
			if (!ctx.catalog->cancelMeeting(meetingId)) {
				const QString msg = ctx.catalog->lastWriteResult().error.message;
				return makeSimpleOutcome(makeErrorResult(name, msg.isEmpty() ? QStringLiteral("Failed to cancel meeting.") : msg));
			}
			const auto &writeRes = ctx.catalog->lastWriteResult();
			QJsonObject payload;
			payload.insert(QStringLiteral("meetingId"), meetingId);
			payload.insert(QStringLiteral("inFlight"), writeRes.inFlight);
			ToolExecutionOutcome outcome;
			outcome.toolResult = makeOkResult(name, payload);
			outcome.asyncInFlight = writeRes.inFlight;
			return outcome;
		};
		registerTool(std::move(t));
	}

	// join_or_start_meeting
	{
		RegisteredTool t;
		QJsonObject props;
		props.insert(QStringLiteral("mode"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Mode: 'join' to join an existing meeting by ID, or 'quick_meeting' to start an instant meeting.")));
		props.insert(QStringLiteral("meeting_id"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Required when mode is 'join': meeting ID to join.")));
		props.insert(QStringLiteral("password"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Optional password when joining a password-protected meeting.")));
		props.insert(QStringLiteral("display_name"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Optional display name in the meeting (defaults to account nickname).")));
		props.insert(QStringLiteral("title"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Optional title when mode is 'quick_meeting'.")));
		props.insert(QStringLiteral("duration_minutes"), makeProp(
			QStringLiteral("integer"),
			QStringLiteral("Optional duration in minutes when mode is 'quick_meeting' (15-480, default 60).")));
		t.definition = {
			QStringLiteral("join_or_start_meeting"),
			QStringLiteral("Join an existing meeting by ID or start an instant quick meeting when not currently in a meeting (requires user confirmation)."),
			makeObjectSchema(props, {QStringLiteral("mode")}),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::AccountCatalog,
			AgentConcurrencyGroup::MeetingTransition,
		};
		t.isAvailable = [](const AgentToolViewContext &ctx) {
			return !ctx.meeting || ctx.meeting->state() == MeetingState::Idle;
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &ctx,
				PreparedPendingAction &out, QString &outError) {
			if (ctx.meeting && ctx.meeting->state() != MeetingState::Idle) {
				outError = QStringLiteral("Already in or connecting to a meeting.");
				return false;
			}
			const QString mode = args.value(QStringLiteral("mode")).toString().trimmed().toLower();
			if (mode == QStringLiteral("join")) {
				const QString meetingId = args.value(QStringLiteral("meeting_id")).toString().trimmed();
				if (meetingId.isEmpty()) {
					outError = QStringLiteral("Missing required parameter 'meeting_id' for join mode.");
					return false;
				}
				const QString password = args.value(QStringLiteral("password")).toString();
				const QString displayName = args.value(QStringLiteral("display_name")).toString().trimmed();
				out.normalizedArgs.insert(QStringLiteral("mode"), mode);
				out.normalizedArgs.insert(QStringLiteral("meeting_id"), meetingId);
				if (!password.isEmpty()) {
					out.normalizedArgs.insert(QStringLiteral("password"), password);
				}
				if (!displayName.isEmpty()) {
					out.normalizedArgs.insert(QStringLiteral("display_name"), displayName.left(64));
				}
				out.title = QCoreApplication::translate("MeetingUI", "Join Meeting");
				out.summary = QCoreApplication::translate("MeetingUI", "Join meeting (ID: %1).").arg(meetingId);
				return true;
			}
			if (mode == QStringLiteral("quick_meeting")) {
				const QString title = args.value(QStringLiteral("title")).toString().trimmed();
				const QString effectiveTitle = title.isEmpty()
					? QCoreApplication::translate("MeetingUI", "Quick Meeting")
					: title.left(128);
				const int mins = args.contains(QStringLiteral("duration_minutes"))
					? args.value(QStringLiteral("duration_minutes")).toInt(60)
					: 60;
				if (mins < 15 || mins > 480) {
					outError = QStringLiteral("duration_minutes for quick_meeting must be between 15 and 480.");
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("mode"), mode);
				out.normalizedArgs.insert(QStringLiteral("title"), effectiveTitle);
				out.normalizedArgs.insert(QStringLiteral("duration_minutes"), mins);
				out.title = QCoreApplication::translate("MeetingUI", "Start Quick Meeting");
				out.summary = QCoreApplication::translate("MeetingUI", "Title: %1 (%2 min)")
					.arg(effectiveTitle, QString::number(mins));
				return true;
			}
			outError = QStringLiteral("Parameter 'mode' must be 'join' or 'quick_meeting'.");
			return false;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("join_or_start_meeting");
			if (ctx.meeting && ctx.meeting->state() != MeetingState::Idle) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Already in or connecting to a meeting.")));
			}
			const QString mode = args.value(QStringLiteral("mode")).toString();
			QJsonObject payload;
			payload.insert(QStringLiteral("mode"), mode);

			if (mode == QStringLiteral("join")) {
				const QString meetingId = args.value(QStringLiteral("meeting_id")).toString();
				const QString password = args.value(QStringLiteral("password")).toString();
				QString displayName = args.value(QStringLiteral("display_name")).toString().trimmed();
				if (displayName.isEmpty() && ctx.sessionManager) {
					displayName = ctx.sessionManager->nickname();
				}
				if (ctx.hostSettings && ctx.hostSettings->joinMeetingFromLobby) {
					const QJsonObject res = ctx.hostSettings->joinMeetingFromLobby(meetingId, password, displayName);
					if (!res.value(QStringLiteral("ok")).toBool(true)) {
						return makeSimpleOutcome(makeErrorResult(
							name,
							res.value(QStringLiteral("error")).toString(QStringLiteral("Failed to join meeting."))));
					}
					return makeSimpleOutcome(
						makeOkResult(name, res),
						QCoreApplication::translate("MeetingUI", "Executed."));
				}
				if (!ctx.meeting) {
					return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Meeting coordinator is unavailable.")));
				}
				const MediaPreferences prefs = ctx.sessionManager
					? ctx.sessionManager->mediaPreferences()
					: MediaPreferences{};
				const livekit::MeetingEncryptionRequest enc = ctx.sessionManager
					? ctx.sessionManager->meetingEncryptionRequest(MeetingEncryptionEntry::JoinMeeting)
					: livekit::MeetingEncryptionRequest{};
				ctx.meeting->joinMeetingAsync(meetingId, password, displayName, prefs, enc);
				payload.insert(QStringLiteral("meetingId"), meetingId);
				payload.insert(QStringLiteral("joining"), true);
			} else if (mode == QStringLiteral("quick_meeting")) {
				const QString title = args.value(QStringLiteral("title")).toString();
				const int mins = args.value(QStringLiteral("duration_minutes")).toInt(60);
				if (ctx.hostSettings && ctx.hostSettings->startQuickMeetingFromLobby) {
					const QJsonObject res = ctx.hostSettings->startQuickMeetingFromLobby(title, mins);
					if (!res.value(QStringLiteral("ok")).toBool(true)) {
						return makeSimpleOutcome(makeErrorResult(
							name,
							res.value(QStringLiteral("error")).toString(QStringLiteral("Failed to start quick meeting."))));
					}
					return makeSimpleOutcome(
						makeOkResult(name, res),
						QCoreApplication::translate("MeetingUI", "Executed."));
				}
				if (!ctx.meeting) {
					return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Meeting coordinator is unavailable.")));
				}
				const MediaPreferences prefs = ctx.sessionManager
					? ctx.sessionManager->mediaPreferences()
					: MediaPreferences{};
				const livekit::MeetingEncryptionRequest enc = ctx.sessionManager
					? ctx.sessionManager->meetingEncryptionRequest(MeetingEncryptionEntry::QuickMeeting)
					: livekit::MeetingEncryptionRequest{};
				ctx.meeting->createAndJoinQuickMeetingAsync(title, mins * 60, prefs, enc);
				payload.insert(QStringLiteral("title"), title);
				payload.insert(QStringLiteral("starting"), true);
			} else {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Unsupported meeting mode.")));
			}

			return makeSimpleOutcome(
				makeOkResult(name, payload),
				QCoreApplication::translate("MeetingUI", "Executed."));
		};
		registerTool(std::move(t));
	}
}

void MeetingToolRegistry::registerContactTools() {
	const auto buildGroupNameMap = [](const ContactCatalogController &contacts) {
		QHash<QString, QString> idToName;
		for (const auto &g : contacts.groups()) {
			idToName.insert(g.groupId, g.name);
		}
		return idToName;
	};

	const auto findGroupByIdOrName = [](
			const ContactCatalogController &contacts,
			const QString &groupId,
			const QString &groupName,
			QString &outError) -> std::optional<ContactGroup> {
		const QString cleanId = groupId.trimmed();
		const QString cleanName = groupName.trimmed();
		if (!cleanId.isEmpty()) {
			for (const auto &g : contacts.groups()) {
				if (g.groupId == cleanId) {
					return g;
				}
			}
			outError = QStringLiteral("Contact group ID '%1' was not found.").arg(cleanId);
			return std::nullopt;
		}
		if (!cleanName.isEmpty()) {
			std::vector<ContactGroup> matches;
			for (const auto &g : contacts.groups()) {
				if (g.name.trimmed().compare(cleanName, Qt::CaseInsensitive) == 0) {
					matches.push_back(g);
				}
			}
			if (matches.size() == 1) {
				return matches.front();
			}
			if (matches.size() > 1) {
				outError = QStringLiteral("Multiple contact groups match '%1'. Please specify group_id.").arg(cleanName);
				return std::nullopt;
			}
			outError = QStringLiteral("Contact group '%1' was not found.").arg(cleanName);
			return std::nullopt;
		}
		outError = QStringLiteral("Either group_id or group name is required.");
		return std::nullopt;
	};

	const auto formatDepartmentNames = [](const UserProfile &profile, const ContactCatalogController &contacts) {
		if (!profile.departmentNames.isEmpty()) {
			return profile.departmentNames;
		}
		QStringList out;
		for (const QString &deptId : profile.primaryDeptIds) {
			for (const auto &d : contacts.departments()) {
				if (d.deptId == deptId && !d.name.isEmpty()) {
					out.push_back(d.name);
					break;
				}
			}
		}
		return out;
	};

	// 1. Read-only tool: search & filter personal contacts, starred, blocked, groups, org members, and directory cache.
	{
		QJsonObject props;
		props.insert(QStringLiteral("query"), makeProp(QStringLiteral("string"),
			QStringLiteral("Optional keyword to match display name, nickname, remark, account, employee number, email, phone, job title, bio/signature, or department.")));
		props.insert(QStringLiteral("favorites_only"), makeProp(QStringLiteral("boolean"),
			QStringLiteral("Optional. True to return only starred/favorite personal contacts (default false).")));
		props.insert(QStringLiteral("include_blocked"), makeProp(QStringLiteral("boolean"),
			QStringLiteral("Optional. True to include blocked contacts in results (default false).")));
		props.insert(QStringLiteral("blocked_only"), makeProp(QStringLiteral("boolean"),
			QStringLiteral("Optional. True to return only blocked contacts (default false).")));
		props.insert(QStringLiteral("group_id"), makeProp(QStringLiteral("string"),
			QStringLiteral("Optional custom contact group ID to filter personal contacts.")));
		props.insert(QStringLiteral("group_name"), makeProp(QStringLiteral("string"),
			QStringLiteral("Optional custom contact group name to filter personal contacts.")));
		props.insert(QStringLiteral("dept_id"), makeProp(QStringLiteral("string"),
			QStringLiteral("Optional organization department ID to filter members.")));
		props.insert(QStringLiteral("refresh"), makeProp(QStringLiteral("boolean"),
			QStringLiteral("Optional. True to also trigger a background sync of contacts, groups, org tree, and requests.")));
		props.insert(QStringLiteral("limit"), makeProp(QStringLiteral("integer"),
			QStringLiteral("Maximum number of contacts to return (1-50, default 20).")));

		RegisteredTool t;
		t.definition = {
			QStringLiteral("search_contacts"),
			QStringLiteral("Search or list synced personal contacts (including starred, grouped, and blocked lists), cached organization members, and directory search results with profile details and online presence."),
			makeObjectSchema(props),
			AgentToolRisk::ReadOnly,
			AgentToolScope::AccountContacts,
		};
		t.isAvailable = [](const AgentToolViewContext &ctx) {
			return ctx.sessionManager && ctx.sessionManager->isLoggedIn() && ctx.contacts != nullptr;
		};
		t.executeReadOnly = [buildGroupNameMap, findGroupByIdOrName, formatDepartmentNames](
				const QJsonObject &args, const AgentToolViewContext &ctx) {
			const QString name = QStringLiteral("search_contacts");
			if (!ctx.contacts) {
				return makeErrorResult(name, QStringLiteral("Contacts directory is unavailable."));
			}
			const bool triggerRefresh = args.value(QStringLiteral("refresh")).toBool(false);
			bool refreshTriggered = false;
			if (triggerRefresh) {
				refreshTriggered = ctx.contacts->refreshAll();
			}

			const QString q = args.value(QStringLiteral("query")).toString().trimmed().toLower();
			const bool favoritesOnly = args.value(QStringLiteral("favorites_only")).toBool(false);
			const bool blockedOnly = args.value(QStringLiteral("blocked_only")).toBool(false);
			const bool includeBlocked = blockedOnly || args.value(QStringLiteral("include_blocked")).toBool(false);
			const QString rawGroupId = args.value(QStringLiteral("group_id")).toString().trimmed();
			const QString rawGroupName = args.value(QStringLiteral("group_name")).toString().trimmed();
			const QString deptIdFilter = args.value(QStringLiteral("dept_id")).toString().trimmed();
			const int limit = std::clamp(args.value(QStringLiteral("limit")).toInt(20), 1, 50);

			QString resolvedGroupId;
			if (!rawGroupId.isEmpty() || !rawGroupName.isEmpty()) {
				QString groupErr;
				const auto grp = findGroupByIdOrName(*ctx.contacts, rawGroupId, rawGroupName, groupErr);
				if (!grp) {
					QJsonObject payload;
					payload.insert(QStringLiteral("count"), 0);
					payload.insert(QStringLiteral("contacts"), QJsonArray{});
					payload.insert(QStringLiteral("groupNotFound"), true);
					payload.insert(QStringLiteral("errorHint"), groupErr);
					QJsonArray availableGroups;
					for (const auto &g : ctx.contacts->groups()) {
						QJsonObject go;
						go.insert(QStringLiteral("groupId"), g.groupId);
						go.insert(QStringLiteral("name"), g.name);
						availableGroups.append(go);
					}
					payload.insert(QStringLiteral("availableGroups"), availableGroups);
					return makeOkResult(name, payload);
				}
				resolvedGroupId = grp->groupId;
			}

			const QHash<QString, QString> groupNameMap = buildGroupNameMap(*ctx.contacts);
			QJsonArray matches;
			QSet<QString> seen;
			int totalMatches = 0;

			auto appendEntry = [&](
					const UserProfile &profile,
					const std::optional<ContactRelation> &rel,
					bool isLeader) {
				const QString uid = !profile.userId.isEmpty()
					? profile.userId
					: (rel ? rel->targetUserId : QString());
				if (uid.isEmpty() || seen.contains(uid)) {
					return;
				}
				const bool isContact = rel.has_value();
				const bool isFav = rel && rel->isFavorite;
				const bool isBlocked = rel && rel->isBlocked;
				const QString remark = rel ? rel->remark.trimmed() : QString();
				const QStringList groupIds = rel ? rel->groupIds : QStringList{};

				if (blockedOnly && !isBlocked) return;
				if (!includeBlocked && isBlocked) return;
				if (favoritesOnly && !isFav) return;
				if (!resolvedGroupId.isEmpty() && !groupIds.contains(resolvedGroupId)) return;

				const QStringList deptNames = formatDepartmentNames(profile, *ctx.contacts);
				if (!deptIdFilter.isEmpty()) {
					if (!profile.primaryDeptIds.contains(deptIdFilter)) {
						bool memberInDept = false;
						for (const auto &m : ctx.contacts->deptMembers(deptIdFilter)) {
							if (m.profile.userId == uid) {
								memberInDept = true;
								break;
							}
						}
						if (!memberInDept) {
							return;
						}
					}
				}

				if (!q.isEmpty()) {
					const bool hit = uid.toLower().contains(q) ||
						profile.nickname.toLower().contains(q) ||
						profile.account.toLower().contains(q) ||
						profile.employeeNo.toLower().contains(q) ||
						profile.email.toLower().contains(q) ||
						profile.phone.toLower().contains(q) ||
						profile.jobTitle.toLower().contains(q) ||
						profile.signature.toLower().contains(q) ||
						remark.toLower().contains(q) ||
						deptNames.join(QLatin1Char(' ')).toLower().contains(q);
					if (!hit) return;
				}

				seen.insert(uid);
				++totalMatches;
				if (matches.size() >= limit) {
					return;
				}

				QJsonObject item;
				item.insert(QStringLiteral("userId"), uid);
				item.insert(QStringLiteral("displayName"), !remark.isEmpty() ? remark : profile.displayName());
				item.insert(QStringLiteral("nickname"), profile.nickname);
				if (!remark.isEmpty()) item.insert(QStringLiteral("remark"), remark);
				if (!profile.account.isEmpty()) item.insert(QStringLiteral("account"), profile.account);
				if (!profile.employeeNo.isEmpty()) item.insert(QStringLiteral("employeeNo"), profile.employeeNo);
				if (!profile.email.isEmpty()) item.insert(QStringLiteral("email"), profile.email);
				if (!profile.phone.isEmpty()) item.insert(QStringLiteral("phone"), profile.phone);
				if (!profile.jobTitle.isEmpty()) item.insert(QStringLiteral("jobTitle"), profile.jobTitle);
				if (!profile.signature.isEmpty()) item.insert(QStringLiteral("signature"), profile.signature);
				if (!deptNames.isEmpty()) item.insert(QStringLiteral("departments"), deptNames.join(QStringLiteral("/")));
				item.insert(QStringLiteral("isContact"), isContact);
				item.insert(QStringLiteral("isFavorite"), isFav);
				item.insert(QStringLiteral("isBlocked"), isBlocked);
				if (ctx.sessionManager && uid == ctx.sessionManager->userId()) item.insert(QStringLiteral("isSelf"), true);
				if (isLeader) item.insert(QStringLiteral("isLeader"), true);
				if (!groupIds.isEmpty()) {
					QJsonArray gIds;
					QJsonArray gNames;
					for (const QString &gid : groupIds) {
						gIds.append(gid);
						const QString gName = groupNameMap.value(gid);
						if (!gName.isEmpty()) {
							gNames.append(gName);
						}
					}
					item.insert(QStringLiteral("groupIds"), gIds);
					item.insert(QStringLiteral("groupNames"), gNames);
				}
				if (ctx.presence) {
					item.insert(QStringLiteral("presence"), presenceStatusToWire(ctx.presence->statusFor(uid)));
				}
				matches.append(item);
			};

			for (const auto &rel : ctx.contacts->contacts()) {
				UserProfile prof = rel.profile.value_or(UserProfile{});
				if (prof.userId.isEmpty()) {
					if (const auto cached = ctx.contacts->findProfile(rel.targetUserId)) {
						prof = *cached;
					} else {
						prof.userId = rel.targetUserId;
					}
				}
				appendEntry(prof, rel, false);
			}

			const bool personalRelationOnly = favoritesOnly || blockedOnly || !resolvedGroupId.isEmpty();
			if (!personalRelationOnly) {
				if (!deptIdFilter.isEmpty()) {
					for (const auto &m : ctx.contacts->deptMembers(deptIdFilter)) {
						appendEntry(m.profile, ctx.contacts->findRelation(m.profile.userId), m.isLeader);
					}
				} else {
					for (const auto &dept : ctx.contacts->departments()) {
						for (const auto &m : ctx.contacts->deptMembers(dept.deptId)) {
							appendEntry(m.profile, ctx.contacts->findRelation(m.profile.userId), m.isLeader);
						}
					}
					for (const auto &prof : ctx.contacts->searchResults()) {
						appendEntry(prof, ctx.contacts->findRelation(prof.userId), false);
					}
				}
			}

			const bool deptRestricted = !deptIdFilter.isEmpty() && ctx.contacts->isDeptMembersRestricted(deptIdFilter);
			bool remoteSearchTriggered = false;
			if (!q.isEmpty() && totalMatches == 0 && !personalRelationOnly && !deptRestricted) {
				remoteSearchTriggered = ctx.contacts->searchDirectory(q, deptIdFilter, QStringLiteral("all"));
			}

			QJsonObject payload;
			payload.insert(QStringLiteral("count"), matches.size());
			payload.insert(QStringLiteral("totalMatches"), totalMatches);
			payload.insert(QStringLiteral("truncated"), totalMatches > matches.size());
			payload.insert(QStringLiteral("contacts"), matches);
			payload.insert(QStringLiteral("groupCount"), static_cast<int>(ctx.contacts->groups().size()));
			payload.insert(QStringLiteral("hasOrganization"), ctx.contacts->hasOrganization());
			payload.insert(QStringLiteral("orgMemberVisibility"), ctx.contacts->orgMemberVisibility());
			payload.insert(QStringLiteral("departmentCount"), static_cast<int>(ctx.contacts->departments().size()));
			payload.insert(QStringLiteral("pendingRequestCount"), static_cast<qint64>(ctx.contacts->pendingRequestCount()));
			if (deptRestricted) {
				payload.insert(QStringLiteral("membersRestricted"), true);
				payload.insert(QStringLiteral("restrictionNotice"),
					QCoreApplication::translate("MeetingUI", "Your permission only allows viewing members of your own department."));
			}
			if (refreshTriggered) {
				payload.insert(QStringLiteral("refreshTriggered"), true);
			}
			if (remoteSearchTriggered) {
				payload.insert(QStringLiteral("remoteSearchTriggered"), true);
				payload.insert(QStringLiteral("hint"), QStringLiteral(
					"No cached local contact matched; a background directory search has been triggered. "
					"To add an external user by exact account, email, or employee number, you can call manage_contact_request with action='send' and account_or_keyword."));
			}
			return makeOkResult(name, payload);
		};
		registerTool(std::move(t));
	}

	// 2. Read-only tool: list custom contact groups and their members.
	{
		QJsonObject props;
		props.insert(QStringLiteral("include_members"), makeProp(QStringLiteral("boolean"),
			QStringLiteral("Optional. Include each group's member userIds and display names (default true).")));

		RegisteredTool t;
		t.definition = {
			QStringLiteral("list_contact_groups"),
			QStringLiteral("List all custom contact groups with groupId, name, sortOrder, and member list."),
			makeObjectSchema(props),
			AgentToolRisk::ReadOnly,
			AgentToolScope::AccountContacts,
		};
		t.isAvailable = [](const AgentToolViewContext &ctx) {
			return ctx.sessionManager && ctx.sessionManager->isLoggedIn() && ctx.contacts != nullptr;
		};
		t.executeReadOnly = [](const QJsonObject &args, const AgentToolViewContext &ctx) {
			const QString name = QStringLiteral("list_contact_groups");
			if (!ctx.contacts) {
				return makeErrorResult(name, QStringLiteral("Contacts directory is unavailable."));
			}
			const bool includeMembers = args.value(QStringLiteral("include_members")).toBool(true);
			const auto isOnline = [&ctx](const QString &uid) {
				return ctx.presence && ctx.presence->statusFor(uid) != PresenceStatus::Offline;
			};
			QSet<QString> validGroupIds;
			for (const auto &g : ctx.contacts->groups()) {
				if (!g.groupId.isEmpty()) validGroupIds.insert(g.groupId);
			}
			int starredCount = 0;
			int starredOnlineCount = 0;
			int myContactsCount = 0;
			int myContactsOnlineCount = 0;
			int blockedCount = 0;
			int totalContacts = 0;
			for (const auto &rel : ctx.contacts->contacts()) {
				if (rel.isBlocked) {
					++blockedCount;
					continue;
				}
				++totalContacts;
				const bool online = isOnline(rel.targetUserId);
				if (rel.isFavorite) {
					++starredCount;
					if (online) ++starredOnlineCount;
				}
				bool inCustomGroup = false;
				for (const QString &gid : rel.groupIds) {
					if (validGroupIds.contains(gid)) {
						inCustomGroup = true;
						break;
					}
				}
				if (!inCustomGroup) {
					++myContactsCount;
					if (online) ++myContactsOnlineCount;
				}
			}

			QJsonArray groupsArr;
			for (const auto &g : ctx.contacts->groups()) {
				QJsonObject row;
				row.insert(QStringLiteral("groupId"), g.groupId);
				row.insert(QStringLiteral("name"), g.name);
				row.insert(QStringLiteral("sortOrder"), g.sortOrder);
				QJsonArray members;
				int memberCount = 0;
				int onlineCount = 0;
				for (const auto &rel : ctx.contacts->contacts()) {
					if (rel.isBlocked || !rel.groupIds.contains(g.groupId)) {
						continue;
					}
					++memberCount;
					if (isOnline(rel.targetUserId)) {
						++onlineCount;
					}
					if (includeMembers) {
						QJsonObject mo;
						mo.insert(QStringLiteral("userId"), rel.targetUserId);
						mo.insert(QStringLiteral("displayName"), rel.effectiveDisplayName());
						if (ctx.presence) {
							mo.insert(QStringLiteral("presence"), presenceStatusToWire(ctx.presence->statusFor(rel.targetUserId)));
						}
						members.append(mo);
					}
				}
				row.insert(QStringLiteral("memberCount"), memberCount);
				row.insert(QStringLiteral("onlineCount"), onlineCount);
				if (includeMembers) {
					row.insert(QStringLiteral("members"), members);
				}
				groupsArr.append(row);
			}
			QJsonObject payload;
			payload.insert(QStringLiteral("count"), groupsArr.size());
			payload.insert(QStringLiteral("groups"), groupsArr);
			payload.insert(QStringLiteral("totalContacts"), totalContacts);
			payload.insert(QStringLiteral("starredCount"), starredCount);
			payload.insert(QStringLiteral("starredOnlineCount"), starredOnlineCount);
			payload.insert(QStringLiteral("myContactsCount"), myContactsCount);
			payload.insert(QStringLiteral("myContactsOnlineCount"), myContactsOnlineCount);
			payload.insert(QStringLiteral("blockedCount"), blockedCount);
			return makeOkResult(name, payload);
		};
		registerTool(std::move(t));
	}

	// 3. Read-only tool: inspect enterprise organization department tree and department members.
	{
		QJsonObject props;
		props.insert(QStringLiteral("dept_id"), makeProp(QStringLiteral("string"),
			QStringLiteral("Optional department ID to also list its cached members.")));
		props.insert(QStringLiteral("dept_name"), makeProp(QStringLiteral("string"),
			QStringLiteral("Optional department name (used if dept_id is omitted) to list members.")));
		props.insert(QStringLiteral("refresh"), makeProp(QStringLiteral("boolean"),
			QStringLiteral("Optional. True to trigger refreshing the organization tree (and selected department members).")));

		RegisteredTool t;
		t.definition = {
			QStringLiteral("list_org_departments"),
			QStringLiteral("List the enterprise organization department tree, and optionally list members of a specific department."),
			makeObjectSchema(props),
			AgentToolRisk::ReadOnly,
			AgentToolScope::AccountContacts,
		};
		t.isAvailable = [](const AgentToolViewContext &ctx) {
			return ctx.sessionManager && ctx.sessionManager->isLoggedIn() && ctx.contacts != nullptr;
		};
		t.executeReadOnly = [](const QJsonObject &args, const AgentToolViewContext &ctx) {
			const QString name = QStringLiteral("list_org_departments");
			if (!ctx.contacts) {
				return makeErrorResult(name, QStringLiteral("Contacts directory is unavailable."));
			}
			const bool refresh = args.value(QStringLiteral("refresh")).toBool(false);
			bool treeRefreshTriggered = false;
			if (refresh || ctx.contacts->departments().empty()) {
				treeRefreshTriggered = ctx.contacts->refreshOrgTree();
			}

			QString targetDeptId = args.value(QStringLiteral("dept_id")).toString().trimmed();
			const QString targetDeptName = args.value(QStringLiteral("dept_name")).toString().trimmed();

			QJsonArray deptsArr;
			std::optional<OrgDepartment> selectedDept;
			for (const auto &d : ctx.contacts->departments()) {
				QJsonObject row;
				row.insert(QStringLiteral("deptId"), d.deptId);
				if (!d.deptCode.isEmpty()) {
					row.insert(QStringLiteral("deptCode"), d.deptCode);
				}
				row.insert(QStringLiteral("parentId"), d.parentId);
				row.insert(QStringLiteral("name"), d.name);
				if (!d.ancestorPath.isEmpty()) {
					row.insert(QStringLiteral("ancestorPath"), d.ancestorPath);
				}
				row.insert(QStringLiteral("sortOrder"), d.sortOrder);
				row.insert(QStringLiteral("memberVisibility"),
					d.memberVisibility.isEmpty() ? QStringLiteral("inherit") : d.memberVisibility);
				row.insert(QStringLiteral("membersRestricted"), ctx.contacts->isDeptMembersRestricted(d.deptId));
				row.insert(QStringLiteral("cachedMemberCount"), static_cast<int>(ctx.contacts->deptMembers(d.deptId).size()));
				deptsArr.append(row);

				if (!selectedDept) {
					if (!targetDeptId.isEmpty() && d.deptId == targetDeptId) {
						selectedDept = d;
					} else if (targetDeptId.isEmpty() && !targetDeptName.isEmpty() &&
						d.name.trimmed().compare(targetDeptName, Qt::CaseInsensitive) == 0) {
						selectedDept = d;
						targetDeptId = d.deptId;
					}
				}
			}

			QJsonObject payload;
			payload.insert(QStringLiteral("hasOrganization"), !deptsArr.isEmpty());
			payload.insert(QStringLiteral("configured"), !deptsArr.isEmpty());
			payload.insert(QStringLiteral("orgMemberVisibility"), ctx.contacts->orgMemberVisibility());
			payload.insert(QStringLiteral("count"), deptsArr.size());
			payload.insert(QStringLiteral("treeVersion"), static_cast<qint64>(ctx.contacts->treeVersion()));
			payload.insert(QStringLiteral("departments"), deptsArr);
			if (treeRefreshTriggered) {
				payload.insert(QStringLiteral("refreshTriggered"), true);
			}

			if (!targetDeptId.isEmpty()) {
				if (refresh) {
					ctx.contacts->loadDeptMembers(targetDeptId);
				} else {
					ctx.contacts->ensureDeptMembersLoaded(targetDeptId);
				}
				const bool membersLoading = ctx.contacts->isDeptMembersLoading(targetDeptId);
				const bool restrictedAfterLoad = ctx.contacts->isDeptMembersRestricted(targetDeptId);
				if (selectedDept) {
					QJsonObject sel;
					sel.insert(QStringLiteral("deptId"), selectedDept->deptId);
					if (!selectedDept->deptCode.isEmpty()) {
						sel.insert(QStringLiteral("deptCode"), selectedDept->deptCode);
					}
					sel.insert(QStringLiteral("name"), selectedDept->name);
					sel.insert(QStringLiteral("parentId"), selectedDept->parentId);
					sel.insert(QStringLiteral("memberVisibility"),
						selectedDept->memberVisibility.isEmpty() ? QStringLiteral("inherit") : selectedDept->memberVisibility);
					payload.insert(QStringLiteral("selectedDepartment"), sel);
				} else {
					payload.insert(QStringLiteral("selectedDeptId"), targetDeptId);
				}
				const auto loadedMembers = ctx.contacts->deptMembers(targetDeptId);
				QJsonArray membersArr;
				for (const auto &m : loadedMembers) {
					const auto rel = ctx.contacts->findRelation(m.profile.userId);
					QJsonObject mo;
					mo.insert(QStringLiteral("userId"), m.profile.userId);
					mo.insert(QStringLiteral("displayName"),
						(rel && !rel->remark.trimmed().isEmpty()) ? rel->remark.trimmed() : m.profile.displayName());
					mo.insert(QStringLiteral("nickname"), m.profile.nickname);
					if (!m.profile.account.isEmpty()) mo.insert(QStringLiteral("account"), m.profile.account);
					if (!m.profile.employeeNo.isEmpty()) mo.insert(QStringLiteral("employeeNo"), m.profile.employeeNo);
					if (!m.profile.jobTitle.isEmpty()) mo.insert(QStringLiteral("jobTitle"), m.profile.jobTitle);
					if (!m.profile.email.isEmpty()) mo.insert(QStringLiteral("email"), m.profile.email);
					if (!m.profile.phone.isEmpty()) mo.insert(QStringLiteral("phone"), m.profile.phone);
					if (!m.profile.signature.isEmpty()) mo.insert(QStringLiteral("signature"), m.profile.signature);
					mo.insert(QStringLiteral("isLeader"), m.isLeader);
					mo.insert(QStringLiteral("isContact"), rel.has_value());
					if (ctx.sessionManager && m.profile.userId == ctx.sessionManager->userId()) {
						mo.insert(QStringLiteral("isSelf"), true);
					}
					if (ctx.presence) {
						mo.insert(QStringLiteral("presence"), presenceStatusToWire(ctx.presence->statusFor(m.profile.userId)));
					}
					membersArr.append(mo);
				}
				payload.insert(QStringLiteral("members"), membersArr);
				payload.insert(QStringLiteral("memberCount"), membersArr.size());
				payload.insert(QStringLiteral("membersRestricted"), restrictedAfterLoad);
				if (restrictedAfterLoad) {
					payload.insert(QStringLiteral("restrictionNotice"),
						QCoreApplication::translate("MeetingUI", "Your permission only allows viewing members of your own department."));
				} else if (membersLoading && membersArr.isEmpty()) {
					payload.insert(QStringLiteral("membersLoading"), true);
				}
			}
			return makeOkResult(name, payload);
		};
		registerTool(std::move(t));
	}

	// 4. Read-only tool: list incoming and outgoing contact requests.
	{
		QJsonObject props;
		props.insert(QStringLiteral("status"), makeProp(QStringLiteral("string"),
			QStringLiteral("Optional filter: 'all' (default), 'pending', 'accepted', or 'rejected'.")));
		props.insert(QStringLiteral("direction"), makeProp(QStringLiteral("string"),
			QStringLiteral("Optional filter: 'all' (default), 'incoming', or 'outgoing'.")));
		props.insert(QStringLiteral("refresh"), makeProp(QStringLiteral("boolean"),
			QStringLiteral("Optional. True to trigger refreshing contact requests from server.")));

		RegisteredTool t;
		t.definition = {
			QStringLiteral("list_contact_requests"),
			QStringLiteral("List incoming and outgoing contact requests with requestId, status, sender/recipient info, and greeting message."),
			makeObjectSchema(props),
			AgentToolRisk::ReadOnly,
			AgentToolScope::AccountContacts,
		};
		t.isAvailable = [](const AgentToolViewContext &ctx) {
			return ctx.sessionManager && ctx.sessionManager->isLoggedIn() && ctx.contacts != nullptr;
		};
		t.executeReadOnly = [](const QJsonObject &args, const AgentToolViewContext &ctx) {
			const QString name = QStringLiteral("list_contact_requests");
			if (!ctx.contacts || !ctx.sessionManager) {
				return makeErrorResult(name, QStringLiteral("Contacts directory is unavailable."));
			}
			const bool refresh = args.value(QStringLiteral("refresh")).toBool(false);
			bool refreshTriggered = false;
			if (refresh) {
				refreshTriggered = ctx.contacts->refreshRequests();
			}
			const QString statusFilter = args.value(QStringLiteral("status")).toString(QStringLiteral("all")).trimmed().toLower();
			const QString dirFilter = args.value(QStringLiteral("direction")).toString(QStringLiteral("all")).trimmed().toLower();
			const QString me = ctx.sessionManager->userId();

			QJsonArray arr;
			for (const auto &r : ctx.contacts->requests()) {
				const bool isIncoming = (r.toUserId == me);
				const QString direction = isIncoming ? QStringLiteral("incoming") : QStringLiteral("outgoing");
				if (dirFilter != QStringLiteral("all") && !dirFilter.isEmpty() && dirFilter != direction) {
					continue;
				}
				QString statusWire = QStringLiteral("pending");
				if (r.status == ContactRequestStatus::Accepted) statusWire = QStringLiteral("accepted");
				else if (r.status == ContactRequestStatus::Rejected) statusWire = QStringLiteral("rejected");
				if (statusFilter != QStringLiteral("all") && !statusFilter.isEmpty() && statusFilter != statusWire) {
					continue;
				}
				const QString peerId = isIncoming ? r.fromUserId : r.toUserId;
				const auto &peerProf = isIncoming ? r.fromProfile : r.toProfile;
				const QString peerName = peerProf
					? peerProf->displayName()
					: ctx.contacts->displayNameForUser(peerId);
				QJsonObject row;
				row.insert(QStringLiteral("requestId"), r.requestId);
				row.insert(QStringLiteral("direction"), direction);
				row.insert(QStringLiteral("status"), statusWire);
				row.insert(QStringLiteral("fromUserId"), r.fromUserId);
				row.insert(QStringLiteral("toUserId"), r.toUserId);
				row.insert(QStringLiteral("peerUserId"), peerId);
				row.insert(QStringLiteral("peerName"), peerName);
				if (peerProf && !peerProf->account.isEmpty()) {
					row.insert(QStringLiteral("peerAccount"), peerProf->account);
				}
				if (!r.greeting.trimmed().isEmpty()) {
					row.insert(QStringLiteral("greeting"), r.greeting.trimmed());
				}
				if (r.createdAtMs > 0) row.insert(QStringLiteral("createdAtMs"), static_cast<qint64>(r.createdAtMs));
				if (r.handledAtMs > 0) row.insert(QStringLiteral("handledAtMs"), static_cast<qint64>(r.handledAtMs));
				arr.append(row);
			}
			QJsonObject payload;
			payload.insert(QStringLiteral("totalCount"), arr.size());
			payload.insert(QStringLiteral("pendingIncomingCount"), static_cast<qint64>(ctx.contacts->pendingRequestCount()));
			payload.insert(QStringLiteral("requests"), arr);
			if (refreshTriggered) {
				payload.insert(QStringLiteral("refreshTriggered"), true);
			}
			return makeOkResult(name, payload);
		};
		registerTool(std::move(t));
	}

	// 5. Confirmation-gated tool: create, rename, reorder, or delete custom contact groups.
	{
		QJsonObject props;
		props.insert(QStringLiteral("action"), makeProp(QStringLiteral("string"),
			QStringLiteral("Group action: 'create', 'rename', 'reorder', or 'delete'.")));
		props.insert(QStringLiteral("name"), makeProp(QStringLiteral("string"),
			QStringLiteral("For 'create': new group name (1-32 chars). For 'rename': new group name. For 'delete'/'reorder': optional group name if group_id is not provided.")));
		props.insert(QStringLiteral("group_id"), makeProp(QStringLiteral("string"),
			QStringLiteral("Target groupId for 'rename', 'reorder', or 'delete' (from list_contact_groups).")));
		props.insert(QStringLiteral("current_name"), makeProp(QStringLiteral("string"),
			QStringLiteral("Optional existing group name to resolve group_id for 'rename', 'reorder', or 'delete'.")));
		props.insert(QStringLiteral("sort_order"), makeProp(QStringLiteral("integer"),
			QStringLiteral("Required for 'reorder': new non-negative integer sort order.")));

		RegisteredTool t;
		t.definition = {
			QStringLiteral("manage_contact_group"),
			QStringLiteral("Create, rename, reorder, or delete a custom contact group (requires user confirmation)."),
			makeObjectSchema(props, {QStringLiteral("action")}),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::AccountContacts,
			AgentConcurrencyGroup::ContactWrite,
		};
		t.isAvailable = [](const AgentToolViewContext &ctx) {
			return ctx.sessionManager && ctx.sessionManager->isLoggedIn() && ctx.contacts != nullptr;
		};
		t.prepareAction = [findGroupByIdOrName](const QJsonObject &args, const AgentToolViewContext &ctx,
				PreparedPendingAction &out, QString &outError) {
			if (!ctx.contacts) {
				outError = QStringLiteral("Contacts directory is unavailable.");
				return false;
			}
			const QString action = args.value(QStringLiteral("action")).toString().trimmed().toLower();
			if (action == QStringLiteral("create")) {
				const QString groupName = args.value(QStringLiteral("name")).toString().trimmed();
				if (groupName.isEmpty()) {
					outError = QStringLiteral("Group 'name' is required for creating a contact group.");
					return false;
				}
				if (groupName.size() > 32) {
					outError = QStringLiteral("Group name must be at most 32 characters.");
					return false;
				}
				for (const auto &g : ctx.contacts->groups()) {
					if (g.name.trimmed().compare(groupName, Qt::CaseInsensitive) == 0) {
						outError = QStringLiteral("Contact group '%1' already exists (groupId: %2).").arg(g.name, g.groupId);
						return false;
					}
				}
				out.title = QCoreApplication::translate("MeetingUI", "Create Contact Group");
				out.summary = QCoreApplication::translate("MeetingUI", "Create a new contact group named \"%1\".").arg(groupName);
				out.normalizedArgs.insert(QStringLiteral("action"), action);
				out.normalizedArgs.insert(QStringLiteral("name"), groupName);
				return true;
			}
			if (action == QStringLiteral("rename")) {
				const QString newName = args.value(QStringLiteral("name")).toString().trimmed();
				if (newName.isEmpty() || newName.size() > 32) {
					outError = QStringLiteral("New group 'name' must be 1-32 characters.");
					return false;
				}
				const auto grp = findGroupByIdOrName(
					*ctx.contacts,
					args.value(QStringLiteral("group_id")).toString(),
					args.value(QStringLiteral("current_name")).toString(),
					outError);
				if (!grp) {
					return false;
				}
				if (grp->name.trimmed() == newName) {
					outError = QStringLiteral("Group is already named '%1'.").arg(newName);
					return false;
				}
				for (const auto &g : ctx.contacts->groups()) {
					if (g.groupId != grp->groupId && g.name.trimmed().compare(newName, Qt::CaseInsensitive) == 0) {
						outError = QStringLiteral("Another contact group named '%1' already exists.").arg(g.name);
						return false;
					}
				}
				out.title = QCoreApplication::translate("MeetingUI", "Rename Contact Group");
				out.summary = QCoreApplication::translate("MeetingUI", "Rename contact group \"%1\" to \"%2\".")
					.arg(grp->name, newName);
				out.normalizedArgs.insert(QStringLiteral("action"), action);
				out.normalizedArgs.insert(QStringLiteral("group_id"), grp->groupId);
				out.normalizedArgs.insert(QStringLiteral("old_name"), grp->name);
				out.normalizedArgs.insert(QStringLiteral("name"), newName);
				return true;
			}
			if (action == QStringLiteral("reorder")) {
				const QString lookupName = !args.value(QStringLiteral("current_name")).toString().trimmed().isEmpty()
					? args.value(QStringLiteral("current_name")).toString()
					: args.value(QStringLiteral("name")).toString();
				const auto grp = findGroupByIdOrName(
					*ctx.contacts,
					args.value(QStringLiteral("group_id")).toString(),
					lookupName,
					outError);
				if (!grp) {
					return false;
				}
				if (!args.contains(QStringLiteral("sort_order")) || !args.value(QStringLiteral("sort_order")).isDouble()) {
					outError = QStringLiteral("Parameter 'sort_order' (non-negative integer) is required for reorder.");
					return false;
				}
				const int sortOrder = args.value(QStringLiteral("sort_order")).toInt(-1);
				if (sortOrder < 0) {
					outError = QStringLiteral("sort_order must be >= 0.");
					return false;
				}
				out.title = QCoreApplication::translate("MeetingUI", "Reorder Contact Group");
				out.summary = QCoreApplication::translate("MeetingUI", "Set sort order of contact group \"%1\" to %2.")
					.arg(grp->name).arg(sortOrder);
				out.normalizedArgs.insert(QStringLiteral("action"), action);
				out.normalizedArgs.insert(QStringLiteral("group_id"), grp->groupId);
				out.normalizedArgs.insert(QStringLiteral("name"), grp->name);
				out.normalizedArgs.insert(QStringLiteral("sort_order"), sortOrder);
				return true;
			}
			if (action == QStringLiteral("delete")) {
				const QString lookupName = !args.value(QStringLiteral("current_name")).toString().trimmed().isEmpty()
					? args.value(QStringLiteral("current_name")).toString()
					: args.value(QStringLiteral("name")).toString();
				const auto grp = findGroupByIdOrName(
					*ctx.contacts,
					args.value(QStringLiteral("group_id")).toString(),
					lookupName,
					outError);
				if (!grp) {
					return false;
				}
				out.title = QCoreApplication::translate("MeetingUI", "Delete Contact Group");
				out.summary = QCoreApplication::translate(
					"MeetingUI",
					"Delete contact group \"%1\". Contacts in this group will not be removed.")
					.arg(grp->name);
				out.normalizedArgs.insert(QStringLiteral("action"), action);
				out.normalizedArgs.insert(QStringLiteral("group_id"), grp->groupId);
				out.normalizedArgs.insert(QStringLiteral("name"), grp->name);
				return true;
			}
			outError = QStringLiteral("Unsupported group action '%1'. Use 'create', 'rename', 'reorder', or 'delete'.").arg(action);
			return false;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("manage_contact_group");
			if (!ctx.contacts) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Contacts directory is unavailable.")));
			}
			ManageContactGroupRequest req;
			req.action = args.value(QStringLiteral("action")).toString().trimmed().toLower();
			req.groupId = args.value(QStringLiteral("group_id")).toString().trimmed();
			req.name = args.value(QStringLiteral("name")).toString().trimmed();
			req.sortOrder = args.value(QStringLiteral("sort_order")).toInt(0);
			const QString oldName = args.value(QStringLiteral("old_name")).toString().trimmed();

			auto syncOutcome = std::make_shared<std::optional<ToolExecutionOutcome>>();
			auto completeAsync = ctx.completeAsync;
			const bool started = ctx.contacts->manageGroup(req,
				[name, req, oldName, syncOutcome, completeAsync](
						bool ok, const ManageContactGroupResponse &resp, const HttpError &err) {
					ToolExecutionOutcome finalOut;
					if (!ok) {
						const QString msg = err.message.isEmpty()
							? QCoreApplication::translate("MeetingUI", "Failed to update contact group.")
							: err.message;
						finalOut = makeSimpleOutcome(makeErrorResult(name, msg), msg);
					} else {
						QJsonObject payload;
						payload.insert(QStringLiteral("action"), req.action);
						payload.insert(QStringLiteral("groupCount"), static_cast<int>(resp.groups.size()));
						if (resp.group) {
							payload.insert(QStringLiteral("groupId"), resp.group->groupId);
							payload.insert(QStringLiteral("name"), resp.group->name);
							payload.insert(QStringLiteral("sortOrder"), resp.group->sortOrder);
						} else if (!req.groupId.isEmpty()) {
							payload.insert(QStringLiteral("groupId"), req.groupId);
						}
						QString uiStatus;
						QString followup;
						if (req.action == QStringLiteral("create")) {
							const QString createdName = resp.group ? resp.group->name : req.name;
							uiStatus = QCoreApplication::translate("MeetingUI", "Group \"%1\" created.").arg(createdName);
							followup = QCoreApplication::translate("MeetingUI", "Contact group \"%1\" has been created.").arg(createdName);
						} else if (req.action == QStringLiteral("rename")) {
							const QString renamed = resp.group ? resp.group->name : req.name;
							uiStatus = QCoreApplication::translate("MeetingUI", "Group renamed to \"%1\".").arg(renamed);
							followup = oldName.isEmpty()
								? QCoreApplication::translate("MeetingUI", "Contact group has been renamed to \"%1\".").arg(renamed)
								: QCoreApplication::translate("MeetingUI", "Contact group \"%1\" has been renamed to \"%2\".").arg(oldName, renamed);
						} else if (req.action == QStringLiteral("reorder")) {
							uiStatus = QCoreApplication::translate("MeetingUI", "Group order updated.");
							followup = QCoreApplication::translate("MeetingUI", "Contact group \"%1\" sort order has been updated to %2.")
								.arg(req.name).arg(req.sortOrder);
						} else {
							uiStatus = QCoreApplication::translate("MeetingUI", "Group \"%1\" deleted.").arg(req.name);
							followup = QCoreApplication::translate("MeetingUI", "Contact group \"%1\" has been deleted.").arg(req.name);
						}
						finalOut = makeSimpleOutcome(makeOkResult(name, payload), uiStatus, followup);
					}
					*syncOutcome = finalOut;
					if (completeAsync) {
						completeAsync(finalOut);
					}
				});

			if (!started) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Unable to submit contact group request.")));
			}
			if (syncOutcome->has_value()) {
				return **syncOutcome;
			}
			QJsonObject pendingPayload;
			pendingPayload.insert(QStringLiteral("action"), req.action);
			pendingPayload.insert(QStringLiteral("inFlight"), true);
			ToolExecutionOutcome outcome;
			outcome.toolResult = makeOkResult(name, pendingPayload);
			outcome.asyncInFlight = true;
			outcome.uiStatusMessage = QCoreApplication::translate("MeetingUI", "Submitting request...");
			return outcome;
		};
		registerTool(std::move(t));
	}

	// 6. Confirmation-gated tool: update contact relation (star, remark, groups, block/unblock) or remove from contacts.
	{
		QJsonObject props;
		props.insert(QStringLiteral("target_user_id"), makeProp(QStringLiteral("string"),
			QStringLiteral("Target contact userId (obtained from search_contacts or list_org_departments).")));
		props.insert(QStringLiteral("action"), makeProp(QStringLiteral("string"),
			QStringLiteral("Action: 'update' (default) to modify star/remark/groups/block state, or 'remove' to remove from personal contacts.")));
		props.insert(QStringLiteral("is_favorite"), makeProp(QStringLiteral("boolean"),
			QStringLiteral("Optional (for 'update'). True to star/favorite the contact, false to unstar.")));
		props.insert(QStringLiteral("remark"), makeProp(QStringLiteral("string"),
			QStringLiteral("Optional (for 'update'). Personal remark/alias (0-64 characters; empty string clears remark).")));
		props.insert(QStringLiteral("is_blocked"), makeProp(QStringLiteral("boolean"),
			QStringLiteral("Optional (for 'update'). True to block the contact, false to unblock.")));
		props.insert(QStringLiteral("group_ids"), makeArrayProp(QStringLiteral("string"),
			QStringLiteral("Optional (for 'update'). Replace the contact's entire list of assigned group IDs.")));
		props.insert(QStringLiteral("group_names"), makeArrayProp(QStringLiteral("string"),
			QStringLiteral("Optional (for 'update'). Replace the contact's entire list of assigned groups by group name.")));
		props.insert(QStringLiteral("add_group_ids"), makeArrayProp(QStringLiteral("string"),
			QStringLiteral("Optional (for 'update'). Add the contact to these group IDs without removing existing memberships.")));
		props.insert(QStringLiteral("add_group_names"), makeArrayProp(QStringLiteral("string"),
			QStringLiteral("Optional (for 'update'). Add the contact to these groups by name without removing existing memberships.")));
		props.insert(QStringLiteral("remove_group_ids"), makeArrayProp(QStringLiteral("string"),
			QStringLiteral("Optional (for 'update'). Remove the contact from these group IDs.")));
		props.insert(QStringLiteral("remove_group_names"), makeArrayProp(QStringLiteral("string"),
			QStringLiteral("Optional (for 'update'). Remove the contact from these groups by name.")));

		RegisteredTool t;
		t.definition = {
			QStringLiteral("update_contact_relation"),
			QStringLiteral("Update a contact's star/favorite status, remark/alias, group memberships, block/unblock state, or remove the contact from personal contacts (requires user confirmation)."),
			makeObjectSchema(props, {QStringLiteral("target_user_id")}),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::AccountContacts,
			AgentConcurrencyGroup::ContactWrite,
		};
		t.isAvailable = [](const AgentToolViewContext &ctx) {
			return ctx.sessionManager && ctx.sessionManager->isLoggedIn() && ctx.contacts != nullptr;
		};
		t.prepareAction = [buildGroupNameMap, findGroupByIdOrName](const QJsonObject &args, const AgentToolViewContext &ctx,
				PreparedPendingAction &out, QString &outError) {
			if (!ctx.contacts || !ctx.sessionManager) {
				outError = QStringLiteral("Contacts directory is unavailable.");
				return false;
			}
			const QString targetId = args.value(QStringLiteral("target_user_id")).toString().trimmed();
			if (targetId.isEmpty()) {
				outError = QStringLiteral("target_user_id is required.");
				return false;
			}
			if (targetId == ctx.sessionManager->userId()) {
				outError = QStringLiteral("Cannot modify contact relation for your own account.");
				return false;
			}
			const QString action = args.value(QStringLiteral("action")).toString(QStringLiteral("update")).trimmed().toLower();
			const QString displayName = ctx.contacts->displayNameForUser(targetId);
			const auto existingRel = ctx.contacts->findRelation(targetId);

			if (action == QStringLiteral("remove")) {
				if (!existingRel) {
					outError = QStringLiteral("User '%1' is not in your personal contacts.").arg(displayName);
					return false;
				}
				out.title = QCoreApplication::translate("MeetingUI", "Remove Contact");
				out.summary = QCoreApplication::translate("MeetingUI", "Remove %1 from your contacts.").arg(displayName);
				out.normalizedArgs.insert(QStringLiteral("action"), action);
				out.normalizedArgs.insert(QStringLiteral("target_user_id"), targetId);
				out.normalizedArgs.insert(QStringLiteral("display_name"), displayName);
				return true;
			}
			if (action != QStringLiteral("update")) {
				outError = QStringLiteral("Unsupported action '%1'. Use 'update' or 'remove'.").arg(action);
				return false;
			}

			QJsonObject norm;
			norm.insert(QStringLiteral("action"), action);
			norm.insert(QStringLiteral("target_user_id"), targetId);
			norm.insert(QStringLiteral("display_name"), displayName);
			QStringList changes;

			if (args.contains(QStringLiteral("is_favorite")) && !args.value(QStringLiteral("is_favorite")).isNull()) {
				if (!args.value(QStringLiteral("is_favorite")).isBool()) {
					outError = QStringLiteral("Parameter 'is_favorite' must be a boolean.");
					return false;
				}
				const bool fav = args.value(QStringLiteral("is_favorite")).toBool();
				norm.insert(QStringLiteral("is_favorite"), fav);
				changes.push_back(fav
					? QCoreApplication::translate("MeetingUI", "Starred -> Yes")
					: QCoreApplication::translate("MeetingUI", "Starred -> No"));
			}

			if (args.contains(QStringLiteral("remark")) && !args.value(QStringLiteral("remark")).isNull()) {
				if (!args.value(QStringLiteral("remark")).isString()) {
					outError = QStringLiteral("Parameter 'remark' must be a string.");
					return false;
				}
				const QString remark = args.value(QStringLiteral("remark")).toString().trimmed();
				if (remark.size() > 64) {
					outError = QStringLiteral("Remark must be at most 64 characters.");
					return false;
				}
				norm.insert(QStringLiteral("remark"), remark);
				changes.push_back(remark.isEmpty()
					? QCoreApplication::translate("MeetingUI", "Remark -> Cleared")
					: QCoreApplication::translate("MeetingUI", "Remark -> %1").arg(remark));
			}

			if (args.contains(QStringLiteral("is_blocked")) && !args.value(QStringLiteral("is_blocked")).isNull()) {
				if (!args.value(QStringLiteral("is_blocked")).isBool()) {
					outError = QStringLiteral("Parameter 'is_blocked' must be a boolean.");
					return false;
				}
				const bool blocked = args.value(QStringLiteral("is_blocked")).toBool();
				norm.insert(QStringLiteral("is_blocked"), blocked);
				changes.push_back(blocked
					? QCoreApplication::translate("MeetingUI", "Status -> Blocked")
					: QCoreApplication::translate("MeetingUI", "Status -> Unblocked"));
			}

			const bool hasReplaceIds = args.contains(QStringLiteral("group_ids")) && !args.value(QStringLiteral("group_ids")).isNull();
			const bool hasReplaceNames = args.contains(QStringLiteral("group_names")) && !args.value(QStringLiteral("group_names")).isNull();
			const bool hasAddIds = args.contains(QStringLiteral("add_group_ids")) && !args.value(QStringLiteral("add_group_ids")).isNull();
			const bool hasAddNames = args.contains(QStringLiteral("add_group_names")) && !args.value(QStringLiteral("add_group_names")).isNull();
			const bool hasRemoveIds = args.contains(QStringLiteral("remove_group_ids")) && !args.value(QStringLiteral("remove_group_ids")).isNull();
			const bool hasRemoveNames = args.contains(QStringLiteral("remove_group_names")) && !args.value(QStringLiteral("remove_group_names")).isNull();

			if ((hasReplaceIds || hasReplaceNames) && (hasAddIds || hasAddNames || hasRemoveIds || hasRemoveNames)) {
				outError = QStringLiteral("Do not combine group_ids/group_names with add_group_* or remove_group_* in one call.");
				return false;
			}

			auto resolveList = [&](const QJsonValue &val, bool byName, QStringList &outIds) -> bool {
				if (!val.isArray()) {
					outError = QStringLiteral("Group parameter must be an array of strings.");
					return false;
				}
				for (const auto &v : val.toArray()) {
					const QString token = v.toString().trimmed();
					if (token.isEmpty()) continue;
					const auto grp = findGroupByIdOrName(*ctx.contacts, byName ? QString() : token, byName ? token : QString(), outError);
					if (!grp) {
						return false;
					}
					if (!outIds.contains(grp->groupId)) {
						outIds.push_back(grp->groupId);
					}
				}
				return true;
			};

			if (hasReplaceIds || hasReplaceNames || hasAddIds || hasAddNames || hasRemoveIds || hasRemoveNames) {
				QStringList nextGroups;
				if (hasReplaceIds || hasReplaceNames) {
					if (hasReplaceIds && !resolveList(args.value(QStringLiteral("group_ids")), false, nextGroups)) return false;
					if (hasReplaceNames && !resolveList(args.value(QStringLiteral("group_names")), true, nextGroups)) return false;
				} else {
					nextGroups = existingRel ? existingRel->groupIds : QStringList{};
					QStringList toAdd;
					QStringList toRemove;
					if (hasAddIds && !resolveList(args.value(QStringLiteral("add_group_ids")), false, toAdd)) return false;
					if (hasAddNames && !resolveList(args.value(QStringLiteral("add_group_names")), true, toAdd)) return false;
					if (hasRemoveIds && !resolveList(args.value(QStringLiteral("remove_group_ids")), false, toRemove)) return false;
					if (hasRemoveNames && !resolveList(args.value(QStringLiteral("remove_group_names")), true, toRemove)) return false;
					for (const QString &gid : toAdd) {
						if (!nextGroups.contains(gid)) {
							nextGroups.push_back(gid);
						}
					}
					for (const QString &gid : toRemove) {
						nextGroups.removeAll(gid);
					}
				}
				QJsonArray gidArr;
				QStringList gNames;
				const auto nameMap = buildGroupNameMap(*ctx.contacts);
				for (const QString &gid : nextGroups) {
					gidArr.append(gid);
					gNames.push_back(nameMap.value(gid, gid));
				}
				norm.insert(QStringLiteral("group_ids"), gidArr);
				changes.push_back(gNames.isEmpty()
					? QCoreApplication::translate("MeetingUI", "Groups -> None")
					: QCoreApplication::translate("MeetingUI", "Groups -> %1").arg(gNames.join(QStringLiteral(", "))));
			}

			if (changes.isEmpty()) {
				outError = QStringLiteral("At least one change (is_favorite, remark, is_blocked, or group_ids/group_names/add_group_*/remove_group_*) must be provided.");
				return false;
			}

			out.title = QCoreApplication::translate("MeetingUI", "Update Contact");
			out.summary = QCoreApplication::translate("MeetingUI", "Contact: %1\n%2")
				.arg(displayName, changes.join(QStringLiteral("\n")));
			out.normalizedArgs = norm;
			return true;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("update_contact_relation");
			if (!ctx.contacts) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Contacts directory is unavailable.")));
			}
			const QString targetId = args.value(QStringLiteral("target_user_id")).toString().trimmed();
			const QString action = args.value(QStringLiteral("action")).toString(QStringLiteral("update")).trimmed().toLower();
			const QString displayName = args.value(QStringLiteral("display_name")).toString().trimmed().isEmpty()
				? ctx.contacts->displayNameForUser(targetId)
				: args.value(QStringLiteral("display_name")).toString().trimmed();

			auto syncOutcome = std::make_shared<std::optional<ToolExecutionOutcome>>();
			auto completeAsync = ctx.completeAsync;

			if (action == QStringLiteral("remove")) {
				const bool started = ctx.contacts->removeRelation(targetId,
					[name, targetId, displayName, syncOutcome, completeAsync](
							bool ok, const bool &removed, const HttpError &err) {
						ToolExecutionOutcome finalOut;
						if (!ok || !removed) {
							const QString msg = err.message.isEmpty()
								? QCoreApplication::translate("MeetingUI", "Failed to remove contact.")
								: err.message;
							finalOut = makeSimpleOutcome(makeErrorResult(name, msg), msg);
						} else {
							QJsonObject payload;
							payload.insert(QStringLiteral("action"), QStringLiteral("remove"));
							payload.insert(QStringLiteral("targetUserId"), targetId);
							payload.insert(QStringLiteral("removed"), true);
							finalOut = makeSimpleOutcome(
								makeOkResult(name, payload),
								QCoreApplication::translate("MeetingUI", "Removed %1 from contacts.").arg(displayName),
								QCoreApplication::translate("MeetingUI", "%1 has been removed from your contacts.").arg(displayName));
						}
						*syncOutcome = finalOut;
						if (completeAsync) {
							completeAsync(finalOut);
						}
					});
				if (!started) {
					return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Unable to submit remove contact request.")));
				}
			} else {
				UpsertContactRelationRequest req;
				req.targetUserId = targetId;
				if (args.contains(QStringLiteral("is_favorite")) && args.value(QStringLiteral("is_favorite")).isBool()) {
					req.isFavorite = args.value(QStringLiteral("is_favorite")).toBool();
				}
				if (args.contains(QStringLiteral("remark")) && args.value(QStringLiteral("remark")).isString()) {
					req.remark = args.value(QStringLiteral("remark")).toString();
				}
				if (args.contains(QStringLiteral("is_blocked")) && args.value(QStringLiteral("is_blocked")).isBool()) {
					req.isBlocked = args.value(QStringLiteral("is_blocked")).toBool();
				}
				if (args.contains(QStringLiteral("group_ids")) && args.value(QStringLiteral("group_ids")).isArray()) {
					QStringList gids;
					for (const auto &v : args.value(QStringLiteral("group_ids")).toArray()) {
						const QString gid = v.toString().trimmed();
						if (!gid.isEmpty() && !gids.contains(gid)) {
							gids.push_back(gid);
						}
					}
					req.groupIds = gids;
				}
				const bool started = ctx.contacts->upsertRelation(req,
					[name, targetId, displayName, syncOutcome, completeAsync](
							bool ok, const ContactRelation &rel, const HttpError &err) {
						ToolExecutionOutcome finalOut;
						if (!ok) {
							const QString msg = err.message.isEmpty()
								? QCoreApplication::translate("MeetingUI", "Failed to update contact.")
								: err.message;
							finalOut = makeSimpleOutcome(makeErrorResult(name, msg), msg);
						} else {
							QJsonObject payload;
							payload.insert(QStringLiteral("action"), QStringLiteral("update"));
							payload.insert(QStringLiteral("targetUserId"), rel.targetUserId.isEmpty() ? targetId : rel.targetUserId);
							payload.insert(QStringLiteral("remark"), rel.remark);
							payload.insert(QStringLiteral("isFavorite"), rel.isFavorite);
							payload.insert(QStringLiteral("isBlocked"), rel.isBlocked);
							QJsonArray gids;
							for (const QString &gid : rel.groupIds) {
								gids.append(gid);
							}
							payload.insert(QStringLiteral("groupIds"), gids);
							finalOut = makeSimpleOutcome(
								makeOkResult(name, payload),
								QCoreApplication::translate("MeetingUI", "Updated %1.").arg(displayName),
								QCoreApplication::translate("MeetingUI", "Contact settings for %1 have been updated.").arg(displayName));
						}
						*syncOutcome = finalOut;
						if (completeAsync) {
							completeAsync(finalOut);
						}
					});
				if (!started) {
					return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Unable to submit contact update request.")));
				}
			}

			if (syncOutcome->has_value()) {
				return **syncOutcome;
			}
			QJsonObject pendingPayload;
			pendingPayload.insert(QStringLiteral("action"), action);
			pendingPayload.insert(QStringLiteral("targetUserId"), targetId);
			pendingPayload.insert(QStringLiteral("inFlight"), true);
			ToolExecutionOutcome outcome;
			outcome.toolResult = makeOkResult(name, pendingPayload);
			outcome.asyncInFlight = true;
			outcome.uiStatusMessage = QCoreApplication::translate("MeetingUI", "Submitting request...");
			return outcome;
		};
		registerTool(std::move(t));
	}

	// 7. Confirmation-gated tool: send, accept, or reject contact requests (supports external directory lookup).
	{
		QJsonObject props;
		props.insert(QStringLiteral("action"), makeProp(QStringLiteral("string"),
			QStringLiteral("Request action: 'send', 'accept', or 'reject'.")));
		props.insert(QStringLiteral("target_user_id"), makeProp(QStringLiteral("string"),
			QStringLiteral("For 'send': the known user ID of the intended recipient. Ask the user to select a recipient when only fuzzy candidates are available.")));
		props.insert(QStringLiteral("account_or_keyword"), makeProp(QStringLiteral("string"),
			QStringLiteral("For 'send': exact account, email, employee number, or user ID. Names and partial values do not resolve a request recipient.")));
		props.insert(QStringLiteral("greeting"), makeProp(QStringLiteral("string"),
			QStringLiteral("Optional (for 'send'): greeting message (0-120 characters).")));
		props.insert(QStringLiteral("request_id"), makeProp(QStringLiteral("string"),
			QStringLiteral("For 'accept' or 'reject': target requestId from list_contact_requests.")));
		props.insert(QStringLiteral("from_user_id"), makeProp(QStringLiteral("string"),
			QStringLiteral("Optional (for 'accept' or 'reject'): sender userId to match a pending incoming request if request_id is omitted.")));

		RegisteredTool t;
		t.definition = {
			QStringLiteral("manage_contact_request"),
			QStringLiteral("Send a contact request (by target_user_id or by looking up an external account/email/employee number), or accept/reject a pending incoming contact request (requires user confirmation)."),
			makeObjectSchema(props, {QStringLiteral("action")}),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::AccountContacts,
			AgentConcurrencyGroup::ContactWrite,
		};
		t.isAvailable = [](const AgentToolViewContext &ctx) {
			return ctx.sessionManager && ctx.sessionManager->isLoggedIn() && ctx.contacts != nullptr;
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &ctx,
				PreparedPendingAction &out, QString &outError) {
			if (!ctx.contacts || !ctx.sessionManager) {
				outError = QStringLiteral("Contacts directory is unavailable.");
				return false;
			}
			const QString action = args.value(QStringLiteral("action")).toString().trimmed().toLower();
			const QString me = ctx.sessionManager->userId();

			if (action == QStringLiteral("send")) {
				const QString targetId = args.value(QStringLiteral("target_user_id")).toString().trimmed();
				const QString keyword = args.value(QStringLiteral("account_or_keyword")).toString().trimmed();
				const QString greeting = args.value(QStringLiteral("greeting")).toString().trimmed();
				if (targetId.isEmpty() && keyword.isEmpty()) {
					outError = QStringLiteral("Either 'target_user_id' or 'account_or_keyword' is required to send a contact request.");
					return false;
				}
				if (greeting.size() > 120) {
					outError = QStringLiteral("Greeting message must be at most 120 characters.");
					return false;
				}
				if (!targetId.isEmpty()) {
					if (targetId == me) {
						outError = QStringLiteral("Cannot add yourself as a contact.");
						return false;
					}
					if (const auto rel = ctx.contacts->findRelation(targetId)) {
						if (rel->isBlocked) {
							outError = QStringLiteral("This user is currently blocked. Unblock them first.");
							return false;
						}
						outError = QStringLiteral("%1 is already in your contacts.").arg(rel->effectiveDisplayName());
						return false;
					}
					const QString displayName = ctx.contacts->displayNameForUser(targetId);
					out.title = QCoreApplication::translate("MeetingUI", "Add Contact");
					out.summary = greeting.isEmpty()
						? QCoreApplication::translate("MeetingUI", "Send a contact request to %1. The recipient must accept.").arg(displayName)
						: QCoreApplication::translate("MeetingUI", "Send a contact request to %1.\nGreeting: \"%2\"").arg(displayName, greeting);
					out.normalizedArgs.insert(QStringLiteral("action"), action);
					out.normalizedArgs.insert(QStringLiteral("target_user_id"), targetId);
					out.normalizedArgs.insert(QStringLiteral("display_name"), displayName);
				} else {
					out.title = QCoreApplication::translate("MeetingUI", "Add External Contact");
					out.summary = greeting.isEmpty()
						? QCoreApplication::translate("MeetingUI", "Look up \"%1\" and send a contact request.").arg(keyword)
						: QCoreApplication::translate("MeetingUI", "Look up \"%1\" and send a contact request.\nGreeting: \"%2\"").arg(keyword, greeting);
					out.normalizedArgs.insert(QStringLiteral("action"), action);
					out.normalizedArgs.insert(QStringLiteral("account_or_keyword"), keyword);
				}
				if (!greeting.isEmpty()) {
					out.normalizedArgs.insert(QStringLiteral("greeting"), greeting);
				}
				return true;
			}

			if (action == QStringLiteral("accept") || action == QStringLiteral("reject")) {
				const QString reqId = args.value(QStringLiteral("request_id")).toString().trimmed();
				const QString fromUserId = args.value(QStringLiteral("from_user_id")).toString().trimmed();
				if (reqId.isEmpty() && fromUserId.isEmpty()) {
					outError = QStringLiteral("Either 'request_id' or 'from_user_id' is required.");
					return false;
				}
				std::optional<ContactRequest> matched;
				for (const auto &r : ctx.contacts->requests()) {
					if (!reqId.isEmpty()) {
						if (r.requestId == reqId) {
							matched = r;
							break;
						}
					} else if (r.fromUserId == fromUserId && r.toUserId == me && r.status == ContactRequestStatus::Pending) {
						matched = r;
						break;
					}
				}
				if (!matched) {
					outError = QStringLiteral("Matching pending incoming contact request was not found.");
					return false;
				}
				if (matched->toUserId != me) {
					outError = QStringLiteral("Only incoming contact requests addressed to you can be accepted or declined.");
					return false;
				}
				if (matched->status != ContactRequestStatus::Pending) {
					outError = QStringLiteral("This contact request has already been handled.");
					return false;
				}
				const QString peerName = matched->fromProfile
					? matched->fromProfile->displayName()
					: ctx.contacts->displayNameForUser(matched->fromUserId);
				const bool accept = (action == QStringLiteral("accept"));
				out.title = accept
					? QCoreApplication::translate("MeetingUI", "Accept Contact Request")
					: QCoreApplication::translate("MeetingUI", "Decline Contact Request");
				out.summary = accept
					? QCoreApplication::translate("MeetingUI", "Accept the contact request from %1.").arg(peerName)
					: QCoreApplication::translate("MeetingUI", "Decline the contact request from %1.").arg(peerName);
				out.normalizedArgs.insert(QStringLiteral("action"), action);
				out.normalizedArgs.insert(QStringLiteral("request_id"), matched->requestId);
				out.normalizedArgs.insert(QStringLiteral("peer_name"), peerName);
				return true;
			}

			outError = QStringLiteral("Unsupported contact request action '%1'. Use 'send', 'accept', or 'reject'.").arg(action);
			return false;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("manage_contact_request");
			if (!ctx.contacts || !ctx.sessionManager) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Contacts directory is unavailable.")));
			}
			const QString action = args.value(QStringLiteral("action")).toString().trimmed().toLower();
			auto syncOutcome = std::make_shared<std::optional<ToolExecutionOutcome>>();
			auto completeAsync = ctx.completeAsync;
			const auto finish = [syncOutcome, completeAsync](const ToolExecutionOutcome &finalOut) {
				*syncOutcome = finalOut;
				if (completeAsync) {
					completeAsync(finalOut);
				}
			};

			if (action == QStringLiteral("send")) {
				const QString targetId = args.value(QStringLiteral("target_user_id")).toString().trimmed();
				const QString keyword = args.value(QStringLiteral("account_or_keyword")).toString().trimmed();
				const QString greeting = args.value(QStringLiteral("greeting")).toString().trimmed();
				QPointer<ContactCatalogController> contactsPtr(ctx.contacts);

				auto sendToTarget = [name, greeting, finish, contactsPtr](const QString &uid, const QString &displayName) -> bool {
					if (!contactsPtr) {
						finish(makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Contacts directory is unavailable."))));
						return false;
					}
					return contactsPtr->sendContactRequest(uid, greeting,
						[name, uid, displayName, finish](
								bool ok, const SendContactRequestResponse &resp, const HttpError &err) {
							if (!ok) {
								const QString msg = err.message.isEmpty()
									? QCoreApplication::translate("MeetingUI", "Failed to send contact request.")
									: err.message;
								finish(makeSimpleOutcome(makeErrorResult(name, msg), msg));
								return;
							}
							QJsonObject payload;
							payload.insert(QStringLiteral("action"), QStringLiteral("send"));
							payload.insert(QStringLiteral("targetUserId"), uid);
							payload.insert(QStringLiteral("targetDisplayName"), displayName);
							payload.insert(QStringLiteral("autoAccepted"), resp.autoAccepted);
							if (resp.request && !resp.request->requestId.isEmpty()) {
								payload.insert(QStringLiteral("requestId"), resp.request->requestId);
							}
							const QString msg = resp.autoAccepted
								? QCoreApplication::translate("MeetingUI", "%1 has been added to your contacts.").arg(displayName)
								: QCoreApplication::translate("MeetingUI", "Contact request sent to %1.").arg(displayName);
							finish(makeSimpleOutcome(makeOkResult(name, payload), msg, msg));
						});
				};

				bool started = false;
				if (!targetId.isEmpty()) {
					const QString displayName = args.value(QStringLiteral("display_name")).toString().trimmed().isEmpty()
						? ctx.contacts->displayNameForUser(targetId)
						: args.value(QStringLiteral("display_name")).toString().trimmed();
					started = sendToTarget(targetId, displayName);
				} else {
					started = ctx.contacts->lookupContactRequestRecipient(keyword,
						[name, contactsPtr, sendToTarget, finish](
								bool ok, const std::optional<UserProfile> &recipient, const HttpError &err) {
							if (!ok) {
								const QString msg = err.message.isEmpty()
									? QCoreApplication::translate("MeetingUI", "Directory lookup failed.")
									: err.message;
								finish(makeSimpleOutcome(makeErrorResult(name, msg), msg));
								return;
							}
							if (!recipient) {
								const QString msg = QCoreApplication::translate("MeetingUI", "No matching user found.");
								finish(makeSimpleOutcome(makeErrorResult(name, msg), msg));
								return;
							}
							if (contactsPtr) {
								if (const auto rel = contactsPtr->findRelation(recipient->userId)) {
									const QString msg = rel->isBlocked
										? QCoreApplication::translate("MeetingUI", "This user is currently blocked. Unblock them first.")
										: QCoreApplication::translate("MeetingUI", "%1 is already in your contacts.").arg(recipient->displayName());
									finish(makeSimpleOutcome(makeErrorResult(name, msg), msg));
									return;
								}
							}
							if (!sendToTarget(recipient->userId, recipient->displayName())) {
								const QString msg = QCoreApplication::translate("MeetingUI", "Failed to send contact request.");
								finish(makeSimpleOutcome(makeErrorResult(name, msg), msg));
							}
						});
				}
				if (!started) {
					return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Unable to submit contact request.")));
				}
			} else {
				const QString reqId = args.value(QStringLiteral("request_id")).toString().trimmed();
				const QString peerName = args.value(QStringLiteral("peer_name")).toString().trimmed();
				const bool accept = (action == QStringLiteral("accept"));
				const bool started = ctx.contacts->handleContactRequest(reqId, accept,
					[name, reqId, peerName, accept, finish](
							bool ok, const HandleContactRequestResponse &resp, const HttpError &err) {
						if (!ok) {
							const QString msg = err.message.isEmpty()
								? QCoreApplication::translate("MeetingUI", "Failed to process contact request.")
								: err.message;
							finish(makeSimpleOutcome(makeErrorResult(name, msg), msg));
							return;
						}
						QJsonObject payload;
						payload.insert(QStringLiteral("action"), accept ? QStringLiteral("accept") : QStringLiteral("reject"));
						payload.insert(QStringLiteral("requestId"), resp.request.requestId.isEmpty() ? reqId : resp.request.requestId);
						payload.insert(QStringLiteral("accepted"), accept);
						const QString targetLabel = peerName.isEmpty() ? reqId : peerName;
						const QString uiStatus = accept
							? QCoreApplication::translate("MeetingUI", "Accepted request from %1.").arg(targetLabel)
							: QCoreApplication::translate("MeetingUI", "Declined request from %1.").arg(targetLabel);
						const QString followup = accept
							? QCoreApplication::translate("MeetingUI", "Contact request from %1 has been accepted.").arg(targetLabel)
							: QCoreApplication::translate("MeetingUI", "Contact request from %1 has been declined.").arg(targetLabel);
						finish(makeSimpleOutcome(makeOkResult(name, payload), uiStatus, followup));
					});
				if (!started) {
					return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Unable to process contact request.")));
				}
			}

			if (syncOutcome->has_value()) {
				return **syncOutcome;
			}
			QJsonObject pendingPayload;
			pendingPayload.insert(QStringLiteral("action"), action);
			pendingPayload.insert(QStringLiteral("inFlight"), true);
			ToolExecutionOutcome outcome;
			outcome.toolResult = makeOkResult(name, pendingPayload);
			outcome.asyncInFlight = true;
			outcome.uiStatusMessage = QCoreApplication::translate("MeetingUI", "Submitting request...");
			return outcome;
		};
		registerTool(std::move(t));
	}

	// 8. Confirmation-gated tool: initiate a 1v1 audio/video call or invite into active meeting.
	{
		QJsonObject props;
		props.insert(QStringLiteral("target_user_id"), makeProp(QStringLiteral("string"),
			QStringLiteral("Target userId obtained from search_contacts or list_org_departments.")));
		props.insert(QStringLiteral("media_type"), makeProp(QStringLiteral("string"),
			QStringLiteral("'audio' or 'video' (default 'video'; ignored when inviting into an active meeting).")));

		RegisteredTool t;
		t.definition = {
			QStringLiteral("call_contact"),
			QStringLiteral("Call a contact (1v1 voice/video call when idle, or invite them into the current active meeting). Requires user confirmation."),
			makeObjectSchema(props, {QStringLiteral("target_user_id")}),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::AccountContacts,
			AgentConcurrencyGroup::ContactWrite,
		};
		t.isAvailable = [](const AgentToolViewContext &ctx) {
			return ctx.sessionManager && ctx.sessionManager->isLoggedIn() && ctx.callInvite != nullptr;
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &ctx,
				PreparedPendingAction &out, QString &outError) {
			const QString targetId = args.value(QStringLiteral("target_user_id")).toString().trimmed();
			if (targetId.isEmpty()) {
				outError = QStringLiteral("target_user_id is required.");
				return false;
			}
			if (ctx.sessionManager && targetId == ctx.sessionManager->userId()) {
				outError = QStringLiteral("Cannot place a call to yourself.");
				return false;
			}
			if (ctx.contacts) {
				if (const auto rel = ctx.contacts->findRelation(targetId); rel && rel->isBlocked) {
					outError = QStringLiteral("This contact is blocked. Unblock them before calling.");
					return false;
				}
			}
			const QString media = args.value(QStringLiteral("media_type")).toString(QStringLiteral("video")).trimmed().toLower();
			const QString normalizedMedia = (media == QStringLiteral("audio")) ? QStringLiteral("audio") : QStringLiteral("video");
			const QString displayName = ctx.contacts ? ctx.contacts->displayNameForUser(targetId) : targetId;
			const bool inMeeting = (ctx.meeting && ctx.meeting->isMeetingReady());
			out.title = inMeeting
				? QCoreApplication::translate("MeetingUI", "Invite Contact to Meeting")
				: QCoreApplication::translate("MeetingUI", "Call Contact");
			if (inMeeting) {
				out.summary = QCoreApplication::translate("MeetingUI", "Invite %1 into the active meeting.").arg(displayName);
			} else if (normalizedMedia == QStringLiteral("audio")) {
				out.summary = QCoreApplication::translate("MeetingUI", "Start a voice call with %1.").arg(displayName);
			} else {
				out.summary = QCoreApplication::translate("MeetingUI", "Start a video call with %1.").arg(displayName);
			}
			QJsonObject norm;
			norm.insert(QStringLiteral("target_user_id"), targetId);
			norm.insert(QStringLiteral("media_type"), normalizedMedia);
			norm.insert(QStringLiteral("mode"), inMeeting ? QStringLiteral("meeting_invite") : QStringLiteral("direct_call"));
			norm.insert(QStringLiteral("display_name"), displayName);
			out.normalizedArgs = norm;
			return true;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("call_contact");
			if (!ctx.callInvite) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Call service is unavailable.")));
			}
			const QString targetId = args.value(QStringLiteral("target_user_id")).toString().trimmed();
			const QString displayName = args.value(QStringLiteral("display_name")).toString().trimmed().isEmpty()
				? (ctx.contacts ? ctx.contacts->displayNameForUser(targetId) : targetId)
				: args.value(QStringLiteral("display_name")).toString().trimmed();
			const QString requestedMode = args.value(QStringLiteral("mode")).toString().trimmed().toLower();
			const bool inMeetingNow = (ctx.meeting && ctx.meeting->isMeetingReady());
			const bool inviteMode = (requestedMode == QStringLiteral("meeting_invite")) ||
				(requestedMode.isEmpty() && inMeetingNow);

			if (inviteMode) {
				if (!inMeetingNow && requestedMode == QStringLiteral("meeting_invite")) {
					return makeSimpleOutcome(makeErrorResult(
						name, QCoreApplication::translate("MeetingUI", "No active meeting to invite into.")));
				}
				auto syncOutcome = std::make_shared<std::optional<ToolExecutionOutcome>>();
				auto completeAsync = ctx.completeAsync;
				const bool started = ctx.callInvite->inviteToActiveMeeting(targetId,
					[name, targetId, displayName, syncOutcome, completeAsync](
							bool ok, const CallSession &sess, const HttpError &err) {
						ToolExecutionOutcome finalOut;
						if (!ok) {
							QString msg = err.message;
							if (err.businessCode == kCallBusyCode || err.code == kCallBusyCode) {
								msg = QCoreApplication::translate("MeetingUI", "The contact is currently busy or in another meeting.");
							} else if (err.businessCode == kContactBlockedCode || err.code == kContactBlockedCode) {
								msg = QCoreApplication::translate("MeetingUI", "Unable to reach this user right now.");
							} else if (msg.isEmpty()) {
								msg = QCoreApplication::translate("MeetingUI", "Failed to send meeting invitation.");
							}
							finalOut = makeSimpleOutcome(makeErrorResult(name, msg), msg);
						} else {
							QJsonObject payload;
							payload.insert(QStringLiteral("targetUserId"), targetId);
							payload.insert(QStringLiteral("mode"), QStringLiteral("meeting_invite"));
							if (!sess.callId.isEmpty()) payload.insert(QStringLiteral("callId"), sess.callId);
							if (!sess.meetingId.isEmpty()) payload.insert(QStringLiteral("meetingId"), sess.meetingId);
							payload.insert(QStringLiteral("status"), QStringLiteral("invited"));
							const QString msg = QCoreApplication::translate("MeetingUI", "Sent meeting invitation to %1.").arg(displayName);
							finalOut = makeSimpleOutcome(makeOkResult(name, payload), msg, msg);
						}
						*syncOutcome = finalOut;
						if (completeAsync) {
							completeAsync(finalOut);
						}
					});
				if (!started) {
					return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Unable to send meeting invitation right now.")));
				}
				if (syncOutcome->has_value()) {
					return **syncOutcome;
				}
				QJsonObject pendingPayload;
				pendingPayload.insert(QStringLiteral("targetUserId"), targetId);
				pendingPayload.insert(QStringLiteral("mode"), QStringLiteral("meeting_invite"));
				pendingPayload.insert(QStringLiteral("inFlight"), true);
				ToolExecutionOutcome outcome;
				outcome.toolResult = makeOkResult(name, pendingPayload);
				outcome.asyncInFlight = true;
				outcome.uiStatusMessage = QCoreApplication::translate("MeetingUI", "Sending meeting invitation...");
				return outcome;
			}

			if (inMeetingNow) {
				return makeSimpleOutcome(makeErrorResult(
					name, QCoreApplication::translate("MeetingUI", "Already in an active meeting; invite the contact to the meeting instead.")));
			}

			const QString mediaWire = args.value(QStringLiteral("media_type")).toString() == QStringLiteral("audio")
				? QStringLiteral("audio") : QStringLiteral("video");
			const auto media = (mediaWire == QStringLiteral("audio"))
				? CallMediaType::Audio : CallMediaType::Video;
			const bool started = ctx.callInvite->startDirectCall(targetId, media);
			if (!started) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Unable to initiate call right now.")));
			}
			QJsonObject payload;
			payload.insert(QStringLiteral("targetUserId"), targetId);
			payload.insert(QStringLiteral("mode"), QStringLiteral("direct_call"));
			payload.insert(QStringLiteral("mediaType"), mediaWire);
			payload.insert(QStringLiteral("status"), QStringLiteral("ringing"));
			const QString followup = (media == CallMediaType::Audio)
				? QCoreApplication::translate("MeetingUI", "Started voice call with %1.").arg(displayName)
				: QCoreApplication::translate("MeetingUI", "Started video call with %1.").arg(displayName);
			return makeSimpleOutcome(
				makeOkResult(name, payload),
				QCoreApplication::translate("MeetingUI", "Calling contact..."),
				followup);
		};
		registerTool(std::move(t));
	}

	// 9. Read-only tool: inspect the signed-in user's own personal profile, employee no., and org history.
	{
		QJsonObject props;
		props.insert(QStringLiteral("refresh"), makeProp(QStringLiteral("boolean"),
			QStringLiteral("Optional. True to trigger refreshing your latest profile and organization history from server.")));

		RegisteredTool t;
		t.definition = {
			QStringLiteral("get_my_profile"),
			QStringLiteral("Inspect the signed-in user's own personal profile (account, nickname, employee number, department, job title, email, phone, bio/signature), organization membership visibility policy, and self-only historical employee numbers / organization removal reasons."),
			makeObjectSchema(props),
			AgentToolRisk::ReadOnly,
			AgentToolScope::AccountContacts,
		};
		t.isAvailable = [](const AgentToolViewContext &ctx) {
			return ctx.sessionManager && ctx.sessionManager->isLoggedIn() && ctx.contacts != nullptr;
		};
		t.executeReadOnly = [formatDepartmentNames](const QJsonObject &args, const AgentToolViewContext &ctx) {
			const QString name = QStringLiteral("get_my_profile");
			if (!ctx.contacts || !ctx.sessionManager) {
				return makeErrorResult(name, QStringLiteral("Contacts directory is unavailable."));
			}
			const QString me = ctx.sessionManager->userId();
			const bool refresh = args.value(QStringLiteral("refresh")).toBool(false);
			bool refreshTriggered = false;
			if (refresh || !ctx.contacts->selfProfile().has_value()) {
				refreshTriggered = ctx.contacts->fetchUserProfile(me);
			}

			UserProfile prof;
			if (const auto &selfProf = ctx.contacts->selfProfile()) {
				prof = *selfProf;
			} else if (const auto cached = ctx.contacts->findProfile(me)) {
				prof = *cached;
			} else {
				prof.userId = me;
				prof.nickname = ctx.sessionManager->nickname();
				prof.account = ctx.sessionManager->savedAccount();
			}
			if (prof.userId.isEmpty()) prof.userId = me;
			if (prof.account.isEmpty()) prof.account = ctx.sessionManager->savedAccount();
			if (prof.nickname.isEmpty()) prof.nickname = ctx.sessionManager->nickname();

			const QStringList deptNames = formatDepartmentNames(prof, *ctx.contacts);
			QJsonArray deptArr;
			for (const QString &dn : deptNames) {
				deptArr.append(dn);
			}
			QJsonArray deptIdArr;
			for (const QString &did : prof.primaryDeptIds) {
				deptIdArr.append(did);
			}

			QJsonArray historyArr;
			for (const auto &rec : ctx.contacts->selfOrgHistory()) {
				QJsonObject ho;
				if (!rec.recordId.isEmpty()) ho.insert(QStringLiteral("recordId"), rec.recordId);
				if (!rec.orgCode.isEmpty()) ho.insert(QStringLiteral("orgCode"), rec.orgCode);
				if (!rec.employeeNo.isEmpty()) ho.insert(QStringLiteral("employeeNo"), rec.employeeNo);
				if (!rec.jobTitle.isEmpty()) ho.insert(QStringLiteral("jobTitle"), rec.jobTitle);
				if (!rec.deptNames.isEmpty()) {
					QJsonArray hDepts;
					for (const QString &dn : rec.deptNames) hDepts.append(dn);
					ho.insert(QStringLiteral("departments"), hDepts);
				}
				ho.insert(QStringLiteral("status"), rec.isRemoved() ? QStringLiteral("removed") : QStringLiteral("active"));
				if (rec.joinedAtMs > 0) ho.insert(QStringLiteral("joinedAtMs"), static_cast<qint64>(rec.joinedAtMs));
				if (rec.removedAtMs > 0) ho.insert(QStringLiteral("removedAtMs"), static_cast<qint64>(rec.removedAtMs));
				if (!rec.removedReason.trimmed().isEmpty()) {
					ho.insert(QStringLiteral("removedReason"), rec.removedReason.trimmed());
				}
				historyArr.append(ho);
			}

			QJsonObject editRules;
			editRules.insert(QStringLiteral("editableFields"),
				QJsonArray{QStringLiteral("nickname"), QStringLiteral("email"), QStringLiteral("phone"), QStringLiteral("signature")});
			editRules.insert(QStringLiteral("employeeNoEditable"), false);
			editRules.insert(QStringLiteral("jobTitleEditableBySelf"), false);
			editRules.insert(QStringLiteral("note"), QStringLiteral(
				"Employee No. is auto-assigned on joining an organization, immutable and never reused. "
				"Job Title can only be modified by the organization founder in the Web Org Admin Console. "
				"Nickname, email, phone, and bio/signature can be edited anytime via update_my_profile."));

			QJsonObject payload;
			payload.insert(QStringLiteral("userId"), prof.userId);
			payload.insert(QStringLiteral("account"), prof.account);
			payload.insert(QStringLiteral("nickname"), prof.nickname);
			payload.insert(QStringLiteral("displayName"), prof.displayName());
			payload.insert(QStringLiteral("employeeNo"), prof.employeeNo);
			payload.insert(QStringLiteral("jobTitle"), prof.jobTitle);
			payload.insert(QStringLiteral("email"), prof.email);
			payload.insert(QStringLiteral("phone"), prof.phone);
			payload.insert(QStringLiteral("signature"), prof.signature);
			payload.insert(QStringLiteral("inOrgDirectory"), prof.inOrgDirectory);
			payload.insert(QStringLiteral("departments"), deptArr);
			if (!deptNames.isEmpty()) {
				payload.insert(QStringLiteral("departmentSummary"), deptNames.join(QStringLiteral(" / ")));
			}
			payload.insert(QStringLiteral("primaryDeptIds"), deptIdArr);
			payload.insert(QStringLiteral("hasOrganization"), ctx.contacts->hasOrganization());
			payload.insert(QStringLiteral("orgMemberVisibility"), ctx.contacts->orgMemberVisibility());
			payload.insert(QStringLiteral("orgHistory"), historyArr);
			payload.insert(QStringLiteral("orgHistoryCount"), historyArr.size());
			payload.insert(QStringLiteral("editRules"), editRules);
			payload.insert(QStringLiteral("uiPath"), QStringLiteral("%1 > %2").arg(
				QCoreApplication::translate("MeetingUI", "Account menu"),
				QCoreApplication::translate("MeetingUI", "Personal Profile...")));
			if (refreshTriggered) {
				payload.insert(QStringLiteral("refreshTriggered"), true);
			}
			return makeOkResult(name, payload);
		};
		registerTool(std::move(t));
	}

	// 10. Confirmation-gated tool: update the signed-in user's own personal profile.
	{
		QJsonObject props;
		props.insert(QStringLiteral("nickname"), makeProp(QStringLiteral("string"),
			QStringLiteral("Optional. New display name / nickname (1-64 characters; cannot be empty).")));
		props.insert(QStringLiteral("email"), makeProp(QStringLiteral("string"),
			QStringLiteral("Optional. New email address (0-128 characters; pass empty string to clear).")));
		props.insert(QStringLiteral("phone"), makeProp(QStringLiteral("string"),
			QStringLiteral("Optional. New phone number (0-32 characters; pass empty string to clear).")));
		props.insert(QStringLiteral("signature"), makeProp(QStringLiteral("string"),
			QStringLiteral("Optional. New personal bio / signature (0-128 characters; pass empty string to clear).")));

		RegisteredTool t;
		t.definition = {
			QStringLiteral("update_my_profile"),
			QStringLiteral("Update the signed-in user's own personal profile (nickname, email, phone, and/or bio/signature). Works whether the user belongs to an organization or not. Employee No. cannot be modified and Job Title can only be changed by the organization founder. Requires user confirmation."),
			makeObjectSchema(props),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::AccountContacts,
			AgentConcurrencyGroup::ContactWrite,
		};
		t.isAvailable = [](const AgentToolViewContext &ctx) {
			return ctx.sessionManager && ctx.sessionManager->isLoggedIn() && ctx.contacts != nullptr;
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &ctx,
				PreparedPendingAction &out, QString &outError) {
			if (!ctx.contacts || !ctx.sessionManager) {
				outError = QStringLiteral("Contacts directory is unavailable.");
				return false;
			}
			if (args.contains(QStringLiteral("employee_no")) || args.contains(QStringLiteral("employeeNo"))) {
				outError = QStringLiteral("Employee No. is auto-assigned upon joining an organization and cannot be modified.");
				return false;
			}
			if (args.contains(QStringLiteral("job_title")) || args.contains(QStringLiteral("jobTitle"))) {
				outError = QStringLiteral("Job Title can only be modified by the organization founder in the Web Org Admin Console.");
				return false;
			}

			QJsonObject norm;
			QStringList changes;

			if (args.contains(QStringLiteral("nickname")) && !args.value(QStringLiteral("nickname")).isNull()) {
				if (!args.value(QStringLiteral("nickname")).isString()) {
					outError = QStringLiteral("Parameter 'nickname' must be a string.");
					return false;
				}
				const QString nick = args.value(QStringLiteral("nickname")).toString().trimmed();
				if (nick.isEmpty() || nick.size() > 64) {
					outError = QStringLiteral("Nickname must be 1-64 characters.");
					return false;
				}
				norm.insert(QStringLiteral("nickname"), nick);
				changes.push_back(QCoreApplication::translate("MeetingUI", "Nickname -> %1").arg(nick));
			}
			if (args.contains(QStringLiteral("email")) && !args.value(QStringLiteral("email")).isNull()) {
				if (!args.value(QStringLiteral("email")).isString()) {
					outError = QStringLiteral("Parameter 'email' must be a string.");
					return false;
				}
				const QString email = args.value(QStringLiteral("email")).toString().trimmed();
				if (email.size() > 128) {
					outError = QStringLiteral("Email must be at most 128 characters.");
					return false;
				}
				norm.insert(QStringLiteral("email"), email);
				changes.push_back(email.isEmpty()
					? QCoreApplication::translate("MeetingUI", "Email -> Cleared")
					: QCoreApplication::translate("MeetingUI", "Email -> %1").arg(email));
			}
			if (args.contains(QStringLiteral("phone")) && !args.value(QStringLiteral("phone")).isNull()) {
				if (!args.value(QStringLiteral("phone")).isString()) {
					outError = QStringLiteral("Parameter 'phone' must be a string.");
					return false;
				}
				const QString phone = args.value(QStringLiteral("phone")).toString().trimmed();
				if (phone.size() > 32) {
					outError = QStringLiteral("Phone must be at most 32 characters.");
					return false;
				}
				norm.insert(QStringLiteral("phone"), phone);
				changes.push_back(phone.isEmpty()
					? QCoreApplication::translate("MeetingUI", "Phone -> Cleared")
					: QCoreApplication::translate("MeetingUI", "Phone -> %1").arg(phone));
			}
			const bool hasSig = args.contains(QStringLiteral("signature")) && !args.value(QStringLiteral("signature")).isNull();
			const bool hasBio = !hasSig && args.contains(QStringLiteral("bio")) && !args.value(QStringLiteral("bio")).isNull();
			if (hasSig || hasBio) {
				const QJsonValue sigVal = hasSig ? args.value(QStringLiteral("signature")) : args.value(QStringLiteral("bio"));
				if (!sigVal.isString()) {
					outError = QStringLiteral("Parameter 'signature' must be a string.");
					return false;
				}
				const QString sig = sigVal.toString().trimmed();
				if (sig.size() > 128) {
					outError = QStringLiteral("Bio / signature must be at most 128 characters.");
					return false;
				}
				norm.insert(QStringLiteral("signature"), sig);
				changes.push_back(sig.isEmpty()
					? QCoreApplication::translate("MeetingUI", "Bio -> Cleared")
					: QCoreApplication::translate("MeetingUI", "Bio -> %1").arg(sig));
			}

			if (changes.isEmpty()) {
				outError = QStringLiteral("At least one editable profile field (nickname, email, phone, or signature) must be provided.");
				return false;
			}

			out.title = QCoreApplication::translate("MeetingUI", "Update Personal Profile");
			out.summary = changes.join(QStringLiteral("\n"));
			out.normalizedArgs = norm;
			return true;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("update_my_profile");
			if (!ctx.contacts || !ctx.sessionManager) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Contacts directory is unavailable.")));
			}
			std::optional<QString> nickname;
			std::optional<QString> signature;
			std::optional<QString> email;
			std::optional<QString> phone;
			if (args.contains(QStringLiteral("nickname")) && args.value(QStringLiteral("nickname")).isString()) {
				nickname = args.value(QStringLiteral("nickname")).toString().trimmed();
			}
			if (args.contains(QStringLiteral("signature")) && args.value(QStringLiteral("signature")).isString()) {
				signature = args.value(QStringLiteral("signature")).toString().trimmed();
			}
			if (args.contains(QStringLiteral("email")) && args.value(QStringLiteral("email")).isString()) {
				email = args.value(QStringLiteral("email")).toString().trimmed();
			}
			if (args.contains(QStringLiteral("phone")) && args.value(QStringLiteral("phone")).isString()) {
				phone = args.value(QStringLiteral("phone")).toString().trimmed();
			}

			auto syncOutcome = std::make_shared<std::optional<ToolExecutionOutcome>>();
			auto completeAsync = ctx.completeAsync;
			const bool started = ctx.contacts->updateSelfProfile(nickname, signature, email, phone,
				[name, syncOutcome, completeAsync](bool ok, const UserProfile &prof, const HttpError &err) {
					ToolExecutionOutcome finalOut;
					if (!ok) {
						const QString msg = err.message.isEmpty()
							? QCoreApplication::translate("MeetingUI", "Failed to update personal profile.")
							: err.message;
						finalOut = makeSimpleOutcome(makeErrorResult(name, msg), msg);
					} else {
						QJsonObject payload;
						payload.insert(QStringLiteral("userId"), prof.userId);
						payload.insert(QStringLiteral("nickname"), prof.nickname);
						payload.insert(QStringLiteral("email"), prof.email);
						payload.insert(QStringLiteral("phone"), prof.phone);
						payload.insert(QStringLiteral("signature"), prof.signature);
						if (!prof.employeeNo.isEmpty()) payload.insert(QStringLiteral("employeeNo"), prof.employeeNo);
						if (!prof.jobTitle.isEmpty()) payload.insert(QStringLiteral("jobTitle"), prof.jobTitle);
						finalOut = makeSimpleOutcome(
							makeOkResult(name, payload),
							QCoreApplication::translate("MeetingUI", "Personal profile saved."),
							QCoreApplication::translate("MeetingUI", "Your personal profile has been updated."));
					}
					*syncOutcome = finalOut;
					if (completeAsync) {
						completeAsync(finalOut);
					}
				});
			if (!started) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Unable to submit personal profile update.")));
			}
			if (syncOutcome->has_value()) {
				return **syncOutcome;
			}
			QJsonObject pendingPayload;
			pendingPayload.insert(QStringLiteral("inFlight"), true);
			ToolExecutionOutcome outcome;
			outcome.toolResult = makeOkResult(name, pendingPayload);
			outcome.asyncInFlight = true;
			outcome.uiStatusMessage = QCoreApplication::translate("MeetingUI", "Submitting request...");
			return outcome;
		};
		registerTool(std::move(t));
	}
}

} // namespace OpenMeeting
