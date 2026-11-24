#include "src/agent/meeting_tool_registry.h"

#include "src/agent/agent_coordinator.h"
#include "src/agent/agent_ui_grounding.h"
#include "src/core/meeting_catalog_controller.h"
#include "src/core/meeting_coordinator.h"
#include "src/media/audio_device_enumerator.h"
#include "src/media/camera_enumerator.h"
#include "src/net/meeting_types.h"
#include "src/net/session_manager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QSet>
#include <QtCore/QTimeZone>

#include <algorithm>
#include <cmath>

namespace OpenMeeting {
namespace {

constexpr int kMaxListedParticipants = 50;
constexpr int kDefaultChatLimit = 20;
constexpr int kMaxChatLimit = 50;
constexpr int kMaxChatSummaryTextLength = 500;
constexpr int kMaxOutboundChatLength = 1000;

QJsonObject makeObjectSchema(const QJsonObject &properties, const QStringList &required = {}) {
	QJsonObject schema;
	schema.insert(QStringLiteral("type"), QStringLiteral("object"));
	schema.insert(QStringLiteral("properties"), properties);
	if (!required.isEmpty()) {
		QJsonArray req;
		for (const auto &field : required) {
			req.append(field);
		}
		schema.insert(QStringLiteral("required"), req);
	}
	schema.insert(QStringLiteral("additionalProperties"), false);
	return schema;
}

QJsonObject makeProp(const QString &type, const QString &description) {
	QJsonObject prop;
	prop.insert(QStringLiteral("type"), type);
	prop.insert(QStringLiteral("description"), description);
	return prop;
}

QJsonObject makeArrayProp(const QString &itemType, const QString &description) {
	QJsonObject items;
	items.insert(QStringLiteral("type"), itemType);
	QJsonObject prop;
	prop.insert(QStringLiteral("type"), QStringLiteral("array"));
	prop.insert(QStringLiteral("items"), items);
	prop.insert(QStringLiteral("description"), description);
	return prop;
}

QString connectionQualityLabel(livekit::ConnectionQuality q) {
	switch (q) {
	case livekit::ConnectionQuality::Poor:
		return QStringLiteral("poor");
	case livekit::ConnectionQuality::Good:
		return QStringLiteral("good");
	case livekit::ConnectionQuality::Excellent:
		return QStringLiteral("excellent");
	case livekit::ConnectionQuality::Lost:
		return QStringLiteral("lost");
	case livekit::ConnectionQuality::Unknown:
	default:
		return QStringLiteral("unknown");
	}
}

QString jsonString(const QJsonObject &obj) {
	return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

AgentToolResult makeErrorResult(const QString &toolName, const QString &message) {
	AgentToolResult res;
	res.toolName = toolName;
	res.success = false;
	res.errorMessage = message;
	QJsonObject errObj;
	errObj.insert(QStringLiteral("ok"), false);
	errObj.insert(QStringLiteral("error"), message);
	res.content = jsonString(errObj);
	return res;
}

AgentToolResult makeOkResult(const QString &toolName, const QJsonObject &payload) {
	AgentToolResult res;
	res.toolName = toolName;
	res.success = true;
	QJsonObject obj = payload;
	obj.insert(QStringLiteral("ok"), true);
	res.content = jsonString(obj);
	return res;
}

ToolExecutionOutcome makeSimpleOutcome(
		const AgentToolResult &res,
		const QString &uiStatus = {},
		const QString &assistantFollowup = {}) {
	ToolExecutionOutcome out;
	out.toolResult = res;
	out.asyncInFlight = false;
	out.uiStatusMessage = uiStatus;
	out.assistantFollowupMessage = assistantFollowup;
	return out;
}

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
		QString &outError) {
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
	outReq.settings.disableCameraOnJoin = args.value(QStringLiteral("disable_camera_on_join")).toBool(true);
	outReq.settings.disableMicrophoneOnJoin = args.value(QStringLiteral("disable_microphone_on_join")).toBool(false);
	outReq.settings.canParticipantJoinMeetingEarly = args.value(QStringLiteral("allow_early_join")).toBool(true);

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
	return bookProps;
}

QString mirrorModeToString(VideoMirrorMode mode) {
	switch (mode) {
	case VideoMirrorMode::LocalOnly:
		return QStringLiteral("local_only");
	case VideoMirrorMode::LocalAndRemote:
		return QStringLiteral("local_and_remote");
	case VideoMirrorMode::Off:
	default:
		return QStringLiteral("off");
	}
}

std::optional<VideoMirrorMode> parseMirrorMode(const QString &raw) {
	const QString norm = raw.trimmed().toLower();
	if (norm == QStringLiteral("off") || norm == QStringLiteral("none")) {
		return VideoMirrorMode::Off;
	}
	if (norm == QStringLiteral("local_only") || norm == QStringLiteral("local")) {
		return VideoMirrorMode::LocalOnly;
	}
	if (norm == QStringLiteral("local_and_remote") || norm == QStringLiteral("all") || norm == QStringLiteral("both")) {
		return VideoMirrorMode::LocalAndRemote;
	}
	return std::nullopt;
}

QString participantNameDisplayToString(ParticipantNameDisplayMode mode) {
	switch (mode) {
	case ParticipantNameDisplayMode::Always:
		return QStringLiteral("always");
	case ParticipantNameDisplayMode::OnHover:
		return QStringLiteral("on_hover");
	case ParticipantNameDisplayMode::Auto:
	default:
		return QStringLiteral("auto");
	}
}

std::optional<ParticipantNameDisplayMode> parseParticipantNameDisplay(const QString &raw) {
	const QString norm = raw.trimmed().toLower();
	if (norm == QStringLiteral("auto")) {
		return ParticipantNameDisplayMode::Auto;
	}
	if (norm == QStringLiteral("always")) {
		return ParticipantNameDisplayMode::Always;
	}
	if (norm == QStringLiteral("on_hover") || norm == QStringLiteral("hover")) {
		return ParticipantNameDisplayMode::OnHover;
	}
	return std::nullopt;
}

QString screenShareResolutionToString(int res) {
	switch (static_cast<livekit::ScreenShareResolution>(res)) {
	case livekit::ScreenShareResolution::P720:
		return QStringLiteral("720p");
	case livekit::ScreenShareResolution::P1080:
		return QStringLiteral("1080p");
	case livekit::ScreenShareResolution::P1440:
		return QStringLiteral("1440p");
	case livekit::ScreenShareResolution::Native:
		return QStringLiteral("native");
	case livekit::ScreenShareResolution::P2160:
		return QStringLiteral("2160p");
	case livekit::ScreenShareResolution::Auto:
	default:
		return QStringLiteral("auto");
	}
}

std::optional<livekit::ScreenShareResolution> parseScreenShareResolution(const QString &raw) {
	const QString norm = raw.trimmed().toLower();
	if (norm == QStringLiteral("auto")) return livekit::ScreenShareResolution::Auto;
	if (norm == QStringLiteral("720p") || norm == QStringLiteral("p720")) return livekit::ScreenShareResolution::P720;
	if (norm == QStringLiteral("1080p") || norm == QStringLiteral("p1080")) return livekit::ScreenShareResolution::P1080;
	if (norm == QStringLiteral("1440p") || norm == QStringLiteral("p1440")) return livekit::ScreenShareResolution::P1440;
	if (norm == QStringLiteral("native")) return livekit::ScreenShareResolution::Native;
	if (norm == QStringLiteral("2160p") || norm == QStringLiteral("p2160") || norm == QStringLiteral("4k")) {
		return livekit::ScreenShareResolution::P2160;
	}
	return std::nullopt;
}

QString boolDisplayLabel(bool enabled) {
	return localizedBooleanSettingLabel(enabled);
}

VideoEncoderPreferencePlatform currentEncoderPlatform() {
#if defined(Q_OS_LINUX)
	return VideoEncoderPreferencePlatform::Linux;
#else
	return VideoEncoderPreferencePlatform::Windows;
#endif
}

std::optional<QString> parseVideoCodecPreferenceInput(const QString &raw) {
	const QString norm = raw.trimmed().toLower();
	if (norm == QStringLiteral("auto")) return QStringLiteral("auto");
	if (norm == QStringLiteral("vp8")) return QStringLiteral("vp8");
	if (norm == QStringLiteral("vp9")) return QStringLiteral("vp9");
	if (norm == QStringLiteral("av1")) return QStringLiteral("av1");
	if (norm == QStringLiteral("h264") || norm == QStringLiteral("h.264") || norm == QStringLiteral("avc")) {
		return QStringLiteral("h264");
	}
	if (norm == QStringLiteral("h265") || norm == QStringLiteral("h.265") || norm == QStringLiteral("hevc")) {
		return QStringLiteral("h265");
	}
	return std::nullopt;
}

std::optional<QString> parseThemeInput(const QString &raw) {
	const QString norm = raw.trimmed().toLower();
	if (norm == QStringLiteral("system") || norm == QStringLiteral("auto") || norm == QStringLiteral("follow_system")) {
		return QStringLiteral("system");
	}
	if (norm == QStringLiteral("light")) return QStringLiteral("light");
	if (norm == QStringLiteral("dark")) return QStringLiteral("dark");
	if (norm == QStringLiteral("teal")) return QStringLiteral("teal");
	return std::nullopt;
}

std::optional<QString> parseLanguageInput(const QString &raw) {
	const QString norm = raw.trimmed().toLower();
	if (norm == QStringLiteral("system") || norm == QStringLiteral("auto") || norm == QStringLiteral("follow_system")) {
		return QStringLiteral("system");
	}
	if (norm == QStringLiteral("zh_cn") || norm == QStringLiteral("zh-cn") ||
		norm == QStringLiteral("zh") || norm == QStringLiteral("chinese")) {
		return QStringLiteral("zh_CN");
	}
	if (norm == QStringLiteral("en_us") || norm == QStringLiteral("en-us") ||
		norm == QStringLiteral("en") || norm == QStringLiteral("english")) {
		return QStringLiteral("en_US");
	}
	return std::nullopt;
}

QString resolveParticipantDisplayName(const MeetingCoordinator *meeting, const QString &userId) {
	if (!meeting || userId.isEmpty()) {
		return userId;
	}
	for (const auto &p : meeting->participants()) {
		if (p.identity == userId) {
			return p.name.trimmed().isEmpty() ? userId : p.name.trimmed();
		}
	}
	return userId;
}

QJsonObject serializeClientPreferences(
		const SessionManager &sessionManager,
		const AgentCoordinator *agentCoordinator = nullptr,
		const AgentHostSettingsBridge *hostSettings = nullptr) {
	const auto &prefs = sessionManager.mediaPreferences();
	const auto &sec = sessionManager.meetingSecurityPreferences();
	QJsonObject payload;
	payload.insert(QStringLiteral("enableMicrophoneOnJoin"), prefs.enableMicrophone);
	payload.insert(QStringLiteral("defaultMutedOnJoin"), !prefs.enableMicrophone);
	payload.insert(QStringLiteral("enableCameraOnJoin"), prefs.enableVideo);
	payload.insert(QStringLiteral("enableSpeakerOnJoin"), prefs.enableSpeaker);
	payload.insert(QStringLiteral("pushToTalkWhenMuted"), prefs.pushToTalkWhenMuted);
	payload.insert(QStringLiteral("echoCancellation"), prefs.echoCancellation);
	payload.insert(QStringLiteral("noiseSuppression"), prefs.noiseSuppression);
	payload.insert(QStringLiteral("autoGainControl"), prefs.autoGainControl);
	payload.insert(QStringLiteral("cameraDeviceId"), prefs.cameraDeviceId);
	payload.insert(QStringLiteral("microphoneDeviceId"), prefs.microphoneDeviceId);
	payload.insert(QStringLiteral("speakerDeviceId"), prefs.speakerDeviceId);
	payload.insert(QStringLiteral("mirrorMode"), mirrorModeToString(prefs.mirrorMode));
	payload.insert(QStringLiteral("receiveResolutionMatch"),
		QString::fromLatin1(livekit::VideoResolutionMatchName(prefs.receiveResolutionMatch)));
	payload.insert(QStringLiteral("videoEncoderPreference"), prefs.videoEncoderPreference);
	payload.insert(QStringLiteral("cameraVideoCodec"), prefs.cameraVideoCodec);
	payload.insert(QStringLiteral("screenShareVideoCodec"), prefs.screenShareVideoCodec);
	payload.insert(QStringLiteral("videoCaptureWidth"), prefs.videoCaptureWidth);
	payload.insert(QStringLiteral("videoCaptureHeight"), prefs.videoCaptureHeight);
	payload.insert(QStringLiteral("videoCaptureFps"), prefs.videoCaptureFps);
	payload.insert(QStringLiteral("screenShareResolution"),
		screenShareResolutionToString(prefs.screenShareResolution));
	payload.insert(QStringLiteral("screenShareFps"), prefs.screenShareFps);
	payload.insert(QStringLiteral("showActiveSpeaker"), prefs.showActiveSpeaker);
	payload.insert(QStringLiteral("participantNameDisplay"),
		participantNameDisplayToString(prefs.participantNameDisplay));
	payload.insert(QStringLiteral("stayInMeetingWhenLocked"), prefs.stayInMeetingWhenLocked);
	payload.insert(QStringLiteral("quitOnMainWindowClose"), prefs.quitOnMainWindowClose);
	payload.insert(QStringLiteral("quickMeetingsE2ee"), sec.quickMeetingsE2ee);
	payload.insert(QStringLiteral("screenShareE2ee"), sec.screenShareE2ee);
	payload.insert(QStringLiteral("meetingDetailsE2ee"), sec.meetingDetailsE2ee);
	payload.insert(QStringLiteral("allMeetingsE2ee"), sec.allMeetingsE2ee);
	payload.insert(QStringLiteral("hasMeetingEncryptionKey"), sessionManager.hasMeetingEncryptionKey());
	if (hostSettings) {
		if (hostSettings->getTheme) {
			payload.insert(QStringLiteral("theme"), hostSettings->getTheme());
		}
		if (hostSettings->getLanguage) {
			payload.insert(QStringLiteral("language"), hostSettings->getLanguage());
		}
	}
	if (agentCoordinator) {
		const auto &ap = agentCoordinator->profile();
		payload.insert(QStringLiteral("copilotProfileName"), ap.name);
		payload.insert(QStringLiteral("copilotTemperature"), ap.temperature);
		payload.insert(QStringLiteral("copilotMaxContextMessages"), ap.maxContextMessages);
		payload.insert(QStringLiteral("copilotSendMeetingContextByDefault"), ap.sendMeetingContextByDefault);
	}
	payload.insert(QStringLiteral("uiDetails"), buildClientPreferencesUiDetails(payload));
	return payload;
}

} // namespace

MeetingToolRegistry::MeetingToolRegistry() {
	registerMeetingControlTools();
	registerHostModerationTools();
	registerCatalogTools();
	registerClientSettingsTools();
}

void MeetingToolRegistry::registerTool(RegisteredTool tool) {
	const QString &name = tool.definition.name;
	if (name.isEmpty()) {
		return;
	}
	if (!_registry.contains(name)) {
		_toolOrder.push_back(name);
	}
	_registry.insert(name, std::move(tool));
}

QVector<AgentToolDefinition> MeetingToolRegistry::toolDefinitions(
		const AgentToolViewContext &ctx) const {
	QVector<AgentToolDefinition> result;
	for (const QString &name : _toolOrder) {
		const auto it = _registry.constFind(name);
		if (it == _registry.constEnd() || !it->exposedInToolDefinitions) {
			continue;
		}
		const auto scope = it->definition.scope;
		bool scopeEnabled = false;
		switch (scope) {
		case AgentToolScope::ClientGlobal:
			scopeEnabled = ctx.canUseClientPrefTools;
			break;
		case AgentToolScope::AccountCatalog:
			scopeEnabled = ctx.canUseCatalogTools;
			break;
		case AgentToolScope::ActiveMeeting:
			scopeEnabled = ctx.canShareMeetingData;
			break;
		case AgentToolScope::MeetingHost:
			scopeEnabled = ctx.canShareMeetingData;
			break;
		}
		if (!scopeEnabled) {
			continue;
		}
		if (it->isAvailable && !it->isAvailable(ctx)) {
			continue;
		}
		result.push_back(it->definition);
	}
	return result;
}

QVector<AgentToolDefinition> MeetingToolRegistry::toolDefinitions(
		bool includeMeetingTools,
		bool includeCatalogTools,
		bool includeClientPreferenceTools) const {
	QVector<AgentToolDefinition> result;
	for (const QString &name : _toolOrder) {
		const auto it = _registry.constFind(name);
		if (it == _registry.constEnd() || !it->exposedInToolDefinitions) {
			continue;
		}
		const auto scope = it->definition.scope;
		if ((scope == AgentToolScope::AccountCatalog && includeCatalogTools) ||
			(scope == AgentToolScope::ClientGlobal && includeClientPreferenceTools) ||
			((scope == AgentToolScope::ActiveMeeting || scope == AgentToolScope::MeetingHost) && includeMeetingTools)) {
			result.push_back(it->definition);
		}
	}
	return result;
}

std::optional<AgentToolDefinition> MeetingToolRegistry::findTool(const QString &name) const {
	const auto it = _registry.constFind(name);
	if (it == _registry.constEnd()) {
		return std::nullopt;
	}
	return it->definition;
}

bool MeetingToolRegistry::isToolAvailable(const QString &name, const AgentToolViewContext &ctx) const {
	const auto it = _registry.constFind(name);
	if (it == _registry.constEnd()) {
		return false;
	}
	if (it->isAvailable) {
		return it->isAvailable(ctx);
	}
	return true;
}

bool MeetingToolRegistry::isReadOnlyTool(const QString &name) const {
	const auto def = findTool(name);
	return def.has_value() && def->risk == AgentToolRisk::ReadOnly;
}

bool MeetingToolRegistry::isCatalogTool(const QString &name) {
	return name == QStringLiteral("list_scheduled_meetings") ||
		name == QStringLiteral("get_scheduled_meeting_detail") ||
		name == QStringLiteral("schedule_meeting") ||
		name == QStringLiteral("update_scheduled_meeting") ||
		name == QStringLiteral("cancel_scheduled_meeting");
}

bool MeetingToolRegistry::isClientPreferenceTool(const QString &name) {
	return name == QStringLiteral("get_join_media_preferences") ||
		name == QStringLiteral("set_join_media_preferences") ||
		name == QStringLiteral("get_client_preferences") ||
		name == QStringLiteral("update_client_preferences");
}

bool MeetingToolRegistry::parseToolArguments(
		const QString &argumentsJson,
		QJsonObject &outArgs,
		QString &outError) {
	outArgs = QJsonObject{};
	outError.clear();
	const QString trimmed = argumentsJson.trimmed();
	if (trimmed.isEmpty()) {
		return true;
	}
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		outError = QStringLiteral("Tool arguments must be a valid JSON object.");
		return false;
	}
	outArgs = doc.object();
	return true;
}

AgentToolResult MeetingToolRegistry::executeReadOnly(
		const QString &name,
		const QJsonObject &args,
		const AgentToolViewContext &ctx) const {
	const auto it = _registry.constFind(name);
	if (it == _registry.constEnd() ||
		it->definition.risk != AgentToolRisk::ReadOnly ||
		!it->executeReadOnly) {
		return makeErrorResult(name, QStringLiteral("Unknown read-only tool."));
	}
	return it->executeReadOnly(args, ctx);
}

AgentToolResult MeetingToolRegistry::executeReadOnly(
		const QString &name,
		const QJsonObject &args,
		const AgentMeetingContext &overview,
		MeetingCoordinator *meeting,
		const QVector<AgentMeetingChatEntry> &recentChat,
		const QVariantMap &telemetrySnapshot,
		MeetingCatalogController *catalog,
		const SessionManager *sessionManager) const {
	AgentToolViewContext ctx;
	ctx.meetingOverview = overview;
	ctx.meeting = meeting;
	ctx.catalog = catalog;
	ctx.sessionManager = sessionManager;
	ctx.recentChat = &recentChat;
	ctx.telemetrySnapshot = &telemetrySnapshot;
	ctx.canShareMeetingData = overview.inMeeting;
	ctx.canUseCatalogTools = (catalog != nullptr);
	ctx.canUseClientPrefTools = (sessionManager != nullptr);
	return executeReadOnly(name, args, ctx);
}

bool MeetingToolRegistry::preparePendingAction(
		const QString &name,
		const QJsonObject &rawArgs,
		const AgentToolViewContext &ctx,
		PreparedPendingAction &outPrepared,
		QString &outError) const {
	outPrepared = PreparedPendingAction{};
	outError.clear();

	const auto it = _registry.constFind(name);
	if (it == _registry.constEnd() ||
		it->definition.risk != AgentToolRisk::RequiresConfirmation ||
		!it->prepareAction) {
		outError = QStringLiteral("Unsupported meeting action tool.");
		return false;
	}
	return it->prepareAction(rawArgs, ctx, outPrepared, outError);
}

bool MeetingToolRegistry::describePendingAction(
		const QString &name,
		const QJsonObject &args,
		QString &outTitle,
		QString &outSummary,
		QString &outError,
		const MeetingCatalogController *catalog,
		const SessionManager *sessionManager,
		MeetingCoordinator *meeting) const {
	AgentToolViewContext ctx;
	ctx.catalog = const_cast<MeetingCatalogController *>(catalog);
	ctx.sessionManager = sessionManager;
	ctx.meeting = meeting;
	PreparedPendingAction prepared;
	if (!preparePendingAction(name, args, ctx, prepared, outError)) {
		outTitle.clear();
		outSummary.clear();
		return false;
	}
	outTitle = prepared.title;
	outSummary = prepared.summary;
	return true;
}

ToolExecutionOutcome MeetingToolRegistry::executeConfirmedActionWithOutcome(
		const QString &name,
		const QJsonObject &normalizedArgs,
		const AgentToolExecuteContext &ctx) const {
	const auto it = _registry.constFind(name);
	if (it == _registry.constEnd() ||
		it->definition.risk != AgentToolRisk::RequiresConfirmation ||
		!it->executeConfirmed) {
		return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Unknown action tool.")));
	}
	return it->executeConfirmed(normalizedArgs, ctx);
}

AgentToolResult MeetingToolRegistry::executeConfirmedAction(
		const QString &name,
		const QJsonObject &args,
		MeetingCoordinator *meeting,
		MeetingCatalogController *catalog,
		SessionManager *sessionManager) const {
	AgentToolExecuteContext ctx;
	ctx.meeting = meeting;
	ctx.catalog = catalog;
	ctx.sessionManager = sessionManager;
	return executeConfirmedActionWithOutcome(name, args, ctx).toolResult;
}

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

QString MeetingToolRegistry::formatJoinMediaPreferencesCompletionSummary(const QJsonObject &args) {
	QStringList parts;
	if (args.contains(QStringLiteral("enable_microphone_on_join")) &&
		args.value(QStringLiteral("enable_microphone_on_join")).isBool()) {
		const bool en = args.value(QStringLiteral("enable_microphone_on_join")).toBool();
		parts.push_back(en
			? QCoreApplication::translate("MeetingUI", "\"Enable microphone when joining a meeting\" is now turned ON")
			: QCoreApplication::translate("MeetingUI", "\"Enable microphone when joining a meeting\" is now turned OFF (muted by default on join)"));
	}
	if (args.contains(QStringLiteral("enable_camera_on_join")) &&
		args.value(QStringLiteral("enable_camera_on_join")).isBool()) {
		const bool en = args.value(QStringLiteral("enable_camera_on_join")).toBool();
		parts.push_back(en
			? QCoreApplication::translate("MeetingUI", "\"Enable camera when joining a meeting\" is now turned ON")
			: QCoreApplication::translate("MeetingUI", "\"Enable camera when joining a meeting\" is now turned OFF"));
	}
	if (args.contains(QStringLiteral("enable_speaker_on_join")) &&
		args.value(QStringLiteral("enable_speaker_on_join")).isBool()) {
		const bool en = args.value(QStringLiteral("enable_speaker_on_join")).toBool();
		parts.push_back(en
			? QCoreApplication::translate("MeetingUI", "\"Use computer audio when joining a meeting\" is now turned ON")
			: QCoreApplication::translate("MeetingUI", "\"Use computer audio when joining a meeting\" is now turned OFF"));
	}
	return QCoreApplication::translate("MeetingUI", "Join media settings updated: %1.")
		.arg(parts.join(QStringLiteral("; ")));
}

ToolInvocationPresentation MeetingToolRegistry::describeToolInvocation(
	const QString &name,
	const QJsonObject &args,
	bool succeeded) {
	struct StaticEntry {
		const char *toolName;
		const char *invocation;
		const char *pastSucceeded;
		const char *pastFailed;
	};
	static const StaticEntry kStaticEntries[] = {
		{ "get_meeting_overview",
		  QT_TRANSLATE_NOOP("MeetingUI", "Reading meeting overview..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Read meeting overview"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to read meeting overview") },
		{ "list_participants",
		  QT_TRANSLATE_NOOP("MeetingUI", "Checking participant list..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Checked participant list"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to check participants") },
		{ "get_recent_chat",
		  QT_TRANSLATE_NOOP("MeetingUI", "Reading recent meeting chat..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Read recent meeting chat"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to read recent meeting chat") },
		{ "get_recent_transcripts",
		  QT_TRANSLATE_NOOP("MeetingUI", "Reading meeting voice transcripts..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Read meeting voice transcripts"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to read meeting transcripts") },
		{ "get_telemetry_snapshot",
		  QT_TRANSLATE_NOOP("MeetingUI", "Reading audio/video telemetry..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Read audio/video telemetry"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to read telemetry") },
		{ "control_collaboration_state",
		  QT_TRANSLATE_NOOP("MeetingUI", "Updating meeting state..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Updated meeting state"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to update meeting state") },
		{ "leave_meeting",
		  QT_TRANSLATE_NOOP("MeetingUI", "Leaving the meeting..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Left the meeting"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to leave the meeting") },
		{ "list_scheduled_meetings",
		  QT_TRANSLATE_NOOP("MeetingUI", "Listing scheduled meetings..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Listed scheduled meetings"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to list scheduled meetings") },
		{ "get_scheduled_meeting_detail",
		  QT_TRANSLATE_NOOP("MeetingUI", "Loading scheduled meeting details..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Loaded scheduled meeting details"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to load meeting details") },
		{ "get_client_preferences",
		  QT_TRANSLATE_NOOP("MeetingUI", "Reading client preferences..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Read client preferences"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to read client preferences") },
		{ "get_join_media_preferences",
		  QT_TRANSLATE_NOOP("MeetingUI", "Reading join media preferences..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Read join media preferences"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to read join media preferences") },
		{ "list_media_devices",
		  QT_TRANSLATE_NOOP("MeetingUI", "Listing audio/video devices..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Listed audio/video devices"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to list media devices") },
		{ "get_app_update_status",
		  QT_TRANSLATE_NOOP("MeetingUI", "Checking app update status..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Checked app update status"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to check update status") },
		{ "check_for_updates",
		  QT_TRANSLATE_NOOP("MeetingUI", "Checking for app updates..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Checked for app updates"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to check for updates") },
		{ "control_app_update",
		  QT_TRANSLATE_NOOP("MeetingUI", "Applying app update action..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Completed app update action"),
		  QT_TRANSLATE_NOOP("MeetingUI", "App update action failed") },
		{ "update_client_preferences",
		  QT_TRANSLATE_NOOP("MeetingUI", "Updating client preferences..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Updated client preferences"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to update client preferences") },
		{ "set_join_media_preferences",
		  QT_TRANSLATE_NOOP("MeetingUI", "Updating join media preferences..."),
		  QT_TRANSLATE_NOOP("MeetingUI", "Updated join media preferences"),
		  QT_TRANSLATE_NOOP("MeetingUI", "Failed to update join media preferences") },
	};

	const auto makePres = [succeeded](const QString &inv, const QString &ok, const QString &fail) {
		return ToolInvocationPresentation{ inv, succeeded ? ok : fail };
	};

	if (name == QLatin1String("set_microphone_muted")) {
		const bool muted = args.value(QStringLiteral("muted")).toBool(true);
		return muted
			? makePres(
				QCoreApplication::translate("MeetingUI", "Muting microphone..."),
				QCoreApplication::translate("MeetingUI", "Muted microphone"),
				QCoreApplication::translate("MeetingUI", "Failed to mute microphone"))
			: makePres(
				QCoreApplication::translate("MeetingUI", "Unmuting microphone..."),
				QCoreApplication::translate("MeetingUI", "Unmuted microphone"),
				QCoreApplication::translate("MeetingUI", "Failed to unmute microphone"));
	}
	if (name == QLatin1String("set_camera_enabled")) {
		const bool enabled = args.value(QStringLiteral("enabled")).toBool(false);
		return enabled
			? makePres(
				QCoreApplication::translate("MeetingUI", "Turning on camera..."),
				QCoreApplication::translate("MeetingUI", "Turned on camera"),
				QCoreApplication::translate("MeetingUI", "Failed to turn on camera"))
			: makePres(
				QCoreApplication::translate("MeetingUI", "Turning off camera..."),
				QCoreApplication::translate("MeetingUI", "Turned off camera"),
				QCoreApplication::translate("MeetingUI", "Failed to turn off camera"));
	}
	if (name == QLatin1String("send_meeting_chat")) {
		return makePres(
			QCoreApplication::translate("MeetingUI", "Sending meeting chat message..."),
			QCoreApplication::translate("MeetingUI", "Sent meeting chat message"),
			QCoreApplication::translate("MeetingUI", "Failed to send meeting chat"));
	}
	if (name == QLatin1String("moderate_participants")) {
		return makePres(
			QCoreApplication::translate("MeetingUI", "Moderating participant..."),
			QCoreApplication::translate("MeetingUI", "Moderated participant"),
			QCoreApplication::translate("MeetingUI", "Failed to moderate participant"));
	}
	if (name == QLatin1String("schedule_meeting")) {
		const QString title = args.value(QStringLiteral("title")).toString().trimmed();
		return title.isEmpty()
			? makePres(
				QCoreApplication::translate("MeetingUI", "Scheduling meeting..."),
				QCoreApplication::translate("MeetingUI", "Scheduled meeting"),
				QCoreApplication::translate("MeetingUI", "Failed to schedule meeting"))
			: makePres(
				QCoreApplication::translate("MeetingUI", "Scheduling meeting \"%1\"...").arg(title),
				QCoreApplication::translate("MeetingUI", "Scheduled meeting \"%1\"").arg(title),
				QCoreApplication::translate("MeetingUI", "Failed to schedule \"%1\"").arg(title));
	}
	if (name == QLatin1String("update_scheduled_meeting")) {
		return makePres(
			QCoreApplication::translate("MeetingUI", "Updating scheduled meeting..."),
			QCoreApplication::translate("MeetingUI", "Updated scheduled meeting"),
			QCoreApplication::translate("MeetingUI", "Failed to update scheduled meeting"));
	}
	if (name == QLatin1String("cancel_scheduled_meeting")) {
		return makePres(
			QCoreApplication::translate("MeetingUI", "Canceling scheduled meeting..."),
			QCoreApplication::translate("MeetingUI", "Canceled scheduled meeting"),
			QCoreApplication::translate("MeetingUI", "Failed to cancel scheduled meeting"));
	}
	if (name == QLatin1String("join_or_start_meeting")) {
		return makePres(
			QCoreApplication::translate("MeetingUI", "Joining or starting meeting..."),
			QCoreApplication::translate("MeetingUI", "Joined or started meeting"),
			QCoreApplication::translate("MeetingUI", "Failed to join or start meeting"));
	}

	for (const StaticEntry &entry : kStaticEntries) {
		if (name == QLatin1String(entry.toolName)) {
			return makePres(
				QCoreApplication::translate("MeetingUI", entry.invocation),
				QCoreApplication::translate("MeetingUI", entry.pastSucceeded),
				QCoreApplication::translate("MeetingUI", entry.pastFailed));
		}
	}

	return makePres(
		QCoreApplication::translate("MeetingUI", "Running %1...").arg(name),
		QCoreApplication::translate("MeetingUI", "Completed %1").arg(name),
		QCoreApplication::translate("MeetingUI", "Tool %1 failed").arg(name));
}

void MeetingToolRegistry::registerMeetingControlTools() {
	// get_meeting_overview
	{
		RegisteredTool t;
		t.definition = {
			QStringLiteral("get_meeting_overview"),
			QStringLiteral("Get a safe summary of the active meeting (ID, title, encryption, participant count, and local media state)."),
			makeObjectSchema({}),
			AgentToolRisk::ReadOnly,
			AgentToolScope::ActiveMeeting,
		};
		t.executeReadOnly = [](const QJsonObject &, const AgentToolViewContext &ctx) {
			if (!ctx.meeting || ctx.meeting->state() == MeetingState::Idle || !ctx.meetingOverview.inMeeting) {
				return makeErrorResult(QStringLiteral("get_meeting_overview"), QStringLiteral("No active meeting session."));
			}
			QJsonObject payload;
			payload.insert(QStringLiteral("meetingId"), ctx.meetingOverview.meetingId);
			payload.insert(QStringLiteral("meetingName"), ctx.meetingOverview.meetingName);
			payload.insert(QStringLiteral("encryptedMeeting"), ctx.meetingOverview.encryptedMeeting);
			payload.insert(QStringLiteral("participantCount"), ctx.meetingOverview.participantCount);
			payload.insert(QStringLiteral("micMuted"), ctx.meetingOverview.micMuted);
			payload.insert(QStringLiteral("cameraEnabled"), ctx.meetingOverview.cameraEnabled);
			payload.insert(QStringLiteral("screenSharing"), ctx.meetingOverview.screenSharing);
			payload.insert(QStringLiteral("isHost"), ctx.meeting->isHost());
			return makeOkResult(QStringLiteral("get_meeting_overview"), payload);
		};
		registerTool(std::move(t));
	}

	// list_participants
	{
		RegisteredTool t;
		t.definition = {
			QStringLiteral("list_participants"),
			QStringLiteral("List current meeting participants with userId, display name, host/local role, mic/camera state, and connection quality."),
			makeObjectSchema({}),
			AgentToolRisk::ReadOnly,
			AgentToolScope::ActiveMeeting,
		};
		t.executeReadOnly = [](const QJsonObject &, const AgentToolViewContext &ctx) {
			if (!ctx.meeting || ctx.meeting->state() == MeetingState::Idle || !ctx.meetingOverview.inMeeting) {
				return makeErrorResult(QStringLiteral("list_participants"), QStringLiteral("No active meeting session."));
			}
			const auto rawList = ctx.meeting->participants();
			std::vector<ParticipantInfo> list;
			list.reserve(rawList.size());
			for (const auto &p : rawList) {
				if (!p.isAgent) {
					list.push_back(p);
				}
			}
			QJsonArray items;
			const int limit = std::min<int>(static_cast<int>(list.size()), kMaxListedParticipants);
			for (int i = 0; i < limit; ++i) {
				const auto &p = list[i];
				QJsonObject row;
				row.insert(QStringLiteral("userId"), p.identity);
				row.insert(QStringLiteral("name"), p.name.trimmed().isEmpty() ? p.identity : p.name.trimmed());
				row.insert(QStringLiteral("isLocal"), p.isLocal);
				row.insert(QStringLiteral("isHost"), p.isHost);
				row.insert(QStringLiteral("micMuted"), p.isAudioMuted);
				row.insert(QStringLiteral("cameraEnabled"), p.isVideoEnabled);
				row.insert(QStringLiteral("speaking"), p.isSpeaking);
				row.insert(QStringLiteral("connectionQuality"), connectionQualityLabel(p.connectionQuality));
				items.append(row);
			}
			QJsonObject payload;
			payload.insert(QStringLiteral("totalCount"), static_cast<int>(list.size()));
			payload.insert(QStringLiteral("truncated"), static_cast<int>(list.size()) > limit);
			payload.insert(QStringLiteral("participants"), items);
			return makeOkResult(QStringLiteral("list_participants"), payload);
		};
		registerTool(std::move(t));
	}

	// get_recent_chat
	{
		RegisteredTool t;
		QJsonObject chatProps;
		chatProps.insert(QStringLiteral("limit"), makeProp(
			QStringLiteral("integer"),
			QStringLiteral("Maximum number of recent chat messages to return (1-50, default 20).")));
		t.definition = {
			QStringLiteral("get_recent_chat"),
			QStringLiteral("Read recent in-meeting text chat messages."),
			makeObjectSchema(chatProps),
			AgentToolRisk::ReadOnly,
			AgentToolScope::ActiveMeeting,
		};
		t.executeReadOnly = [](const QJsonObject &args, const AgentToolViewContext &ctx) {
			if (!ctx.meeting || ctx.meeting->state() == MeetingState::Idle || !ctx.meetingOverview.inMeeting) {
				return makeErrorResult(QStringLiteral("get_recent_chat"), QStringLiteral("No active meeting session."));
			}
			const QVector<AgentMeetingChatEntry> emptyChat;
			const QVector<AgentMeetingChatEntry> &recentChat = ctx.recentChat ? *ctx.recentChat : emptyChat;
			const int rawLimit = args.value(QStringLiteral("limit")).toInt(kDefaultChatLimit);
			const int limit = qBound(1, rawLimit, kMaxChatLimit);
			const int total = recentChat.size();
			const int start = std::max(0, total - limit);
			QJsonArray items;
			for (int i = start; i < total; ++i) {
				const auto &entry = recentChat.at(i);
				QJsonObject row;
				row.insert(QStringLiteral("sender"), entry.senderName);
				row.insert(QStringLiteral("isLocal"), entry.isLocal);
				row.insert(QStringLiteral("text"), entry.text.left(kMaxChatSummaryTextLength));
				if (entry.timestamp.isValid()) {
					row.insert(QStringLiteral("time"), entry.timestamp.toString(QStringLiteral("HH:mm:ss")));
				}
				items.append(row);
			}
			QJsonObject payload;
			payload.insert(QStringLiteral("totalCount"), total);
			payload.insert(QStringLiteral("messages"), items);
			return makeOkResult(QStringLiteral("get_recent_chat"), payload);
		};
		registerTool(std::move(t));
	}

	// get_recent_transcripts
	{
		RegisteredTool t;
		QJsonObject transcriptProps;
		transcriptProps.insert(QStringLiteral("limit"), makeProp(
			QStringLiteral("integer"),
			QStringLiteral("Maximum number of recent voice transcript segments to return (1-50, default 30).")));
		t.definition = {
			QStringLiteral("get_recent_transcripts"),
			QStringLiteral("Read recent live or finalized spoken voice transcripts from participants and AI voice agents in the active meeting."),
			makeObjectSchema(transcriptProps),
			AgentToolRisk::ReadOnly,
			AgentToolScope::ActiveMeeting,
		};
		t.executeReadOnly = [](const QJsonObject &args, const AgentToolViewContext &ctx) {
			if (!ctx.meeting || ctx.meeting->state() == MeetingState::Idle || !ctx.meetingOverview.inMeeting) {
				return makeErrorResult(QStringLiteral("get_recent_transcripts"), QStringLiteral("No active meeting session."));
			}
			const QVector<AgentMeetingTranscriptEntry> emptyTranscripts;
			const QVector<AgentMeetingTranscriptEntry> &recentTranscripts =
				ctx.recentTranscripts ? *ctx.recentTranscripts : emptyTranscripts;
			const int rawLimit = args.value(QStringLiteral("limit")).toInt(30);
			const int limit = qBound(1, rawLimit, kMaxChatLimit);
			const int total = recentTranscripts.size();
			const int start = std::max(0, total - limit);
			QJsonArray items;
			for (int i = start; i < total; ++i) {
				const auto &entry = recentTranscripts.at(i);
				QJsonObject row;
				row.insert(QStringLiteral("speaker"),
					entry.speakerName.trimmed().isEmpty() ? entry.speakerIdentity : entry.speakerName);
				row.insert(QStringLiteral("speakerId"), entry.speakerIdentity);
				row.insert(QStringLiteral("isLocal"), entry.isLocalSpeaker);
				row.insert(QStringLiteral("isAgentGenerated"), entry.isAgentGenerated);
				row.insert(QStringLiteral("isFinal"), entry.isFinal);
				if (!entry.language.isEmpty()) {
					row.insert(QStringLiteral("language"), entry.language);
				}
				row.insert(QStringLiteral("text"), entry.text.left(kMaxChatSummaryTextLength));
				if (entry.timestampMs > 0) {
					row.insert(
						QStringLiteral("time"),
						QDateTime::fromMSecsSinceEpoch(entry.timestampMs).toString(QStringLiteral("HH:mm:ss")));
				}
				items.append(row);
			}
			QJsonObject payload;
			payload.insert(QStringLiteral("totalCount"), total);
			payload.insert(QStringLiteral("transcripts"), items);
			return makeOkResult(QStringLiteral("get_recent_transcripts"), payload);
		};
		registerTool(std::move(t));
	}

	// get_telemetry_snapshot
	{
		RegisteredTool t;
		t.definition = {
			QStringLiteral("get_telemetry_snapshot"),
			QStringLiteral("Read the latest safe projected meeting telemetry snapshot (network quality, RTT, packet loss, bitrate, reconnect stats)."),
			makeObjectSchema({}),
			AgentToolRisk::ReadOnly,
			AgentToolScope::ActiveMeeting,
		};
		t.executeReadOnly = [](const QJsonObject &, const AgentToolViewContext &ctx) {
			if (!ctx.meeting || ctx.meeting->state() == MeetingState::Idle || !ctx.meetingOverview.inMeeting) {
				return makeErrorResult(QStringLiteral("get_telemetry_snapshot"), QStringLiteral("No active meeting session."));
			}
			QJsonObject payload;
			if (!ctx.telemetrySnapshot || ctx.telemetrySnapshot->isEmpty()) {
				payload.insert(QStringLiteral("available"), false);
			} else {
				payload.insert(QStringLiteral("available"), true);
				payload.insert(QStringLiteral("snapshot"), QJsonObject::fromVariantMap(*ctx.telemetrySnapshot));
			}
			return makeOkResult(QStringLiteral("get_telemetry_snapshot"), payload);
		};
		registerTool(std::move(t));
	}

	// set_microphone_muted
	{
		RegisteredTool t;
		QJsonObject micProps;
		micProps.insert(QStringLiteral("muted"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("True to mute the local microphone, false to unmute.")));
		t.definition = {
			QStringLiteral("set_microphone_muted"),
			QStringLiteral("Mute or unmute the local microphone (requires user confirmation)."),
			makeObjectSchema(micProps, {QStringLiteral("muted")}),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::ActiveMeeting,
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &,
				PreparedPendingAction &out, QString &outError) {
			const auto val = args.value(QStringLiteral("muted"));
			if (!val.isBool()) {
				outError = QStringLiteral("Missing boolean parameter 'muted'.");
				return false;
			}
			const bool muted = val.toBool();
			out.title = muted
				? QCoreApplication::translate("MeetingUI", "Mute Microphone")
				: QCoreApplication::translate("MeetingUI", "Unmute Microphone");
			out.summary = muted
				? QCoreApplication::translate("MeetingUI", "Turn off your microphone in the active meeting.")
				: QCoreApplication::translate("MeetingUI", "Turn on your microphone in the active meeting.");
			out.normalizedArgs = args;
			return true;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("set_microphone_muted");
			if (!ctx.meeting || ctx.meeting->state() == MeetingState::Idle) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Meeting session is no longer active.")));
			}
			const auto val = args.value(QStringLiteral("muted"));
			if (!val.isBool()) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Missing boolean parameter 'muted'.")));
			}
			const bool muted = val.toBool();
			ctx.meeting->setLocalAudioMuted(muted);
			QJsonObject payload;
			payload.insert(QStringLiteral("micMuted"), ctx.meeting->isLocalAudioMuted());
			return makeSimpleOutcome(
				makeOkResult(name, payload),
				QCoreApplication::translate("MeetingUI", "Executed."));
		};
		registerTool(std::move(t));
	}

	// set_camera_enabled
	{
		RegisteredTool t;
		QJsonObject camProps;
		camProps.insert(QStringLiteral("enabled"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("True to turn on the local camera, false to turn it off.")));
		t.definition = {
			QStringLiteral("set_camera_enabled"),
			QStringLiteral("Enable or disable the local camera (requires user confirmation)."),
			makeObjectSchema(camProps, {QStringLiteral("enabled")}),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::ActiveMeeting,
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &,
				PreparedPendingAction &out, QString &outError) {
			const auto val = args.value(QStringLiteral("enabled"));
			if (!val.isBool()) {
				outError = QStringLiteral("Missing boolean parameter 'enabled'.");
				return false;
			}
			const bool enabled = val.toBool();
			out.title = enabled
				? QCoreApplication::translate("MeetingUI", "Turn On Camera")
				: QCoreApplication::translate("MeetingUI", "Turn Off Camera");
			out.summary = enabled
				? QCoreApplication::translate("MeetingUI", "Start sending local camera video to the meeting.")
				: QCoreApplication::translate("MeetingUI", "Stop sending local camera video to the meeting.");
			out.normalizedArgs = args;
			return true;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("set_camera_enabled");
			if (!ctx.meeting || ctx.meeting->state() == MeetingState::Idle) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Meeting session is no longer active.")));
			}
			const auto val = args.value(QStringLiteral("enabled"));
			if (!val.isBool()) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Missing boolean parameter 'enabled'.")));
			}
			ctx.meeting->setLocalVideoEnabled(val.toBool());
			QJsonObject payload;
			payload.insert(QStringLiteral("cameraEnabled"), ctx.meeting->isLocalVideoEnabled());
			return makeSimpleOutcome(
				makeOkResult(name, payload),
				QCoreApplication::translate("MeetingUI", "Executed."));
		};
		registerTool(std::move(t));
	}

	// send_meeting_chat
	{
		RegisteredTool t;
		QJsonObject sendChatProps;
		sendChatProps.insert(QStringLiteral("message"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Text message to post to the public meeting chat (1-1000 characters).")));
		t.definition = {
			QStringLiteral("send_meeting_chat"),
			QStringLiteral("Send a public text message to the meeting chat (requires user confirmation)."),
			makeObjectSchema(sendChatProps, {QStringLiteral("message")}),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::ActiveMeeting,
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &,
				PreparedPendingAction &out, QString &outError) {
			const QString msg = args.value(QStringLiteral("message")).toString().trimmed();
			if (msg.isEmpty()) {
				outError = QStringLiteral("Message content must not be empty.");
				return false;
			}
			if (msg.size() > kMaxOutboundChatLength) {
				outError = QStringLiteral("Message exceeds maximum length (1000 characters).");
				return false;
			}
			out.title = QCoreApplication::translate("MeetingUI", "Send Meeting Chat");
			out.summary = QCoreApplication::translate("MeetingUI", "Post to public chat: \"%1\"")
				.arg(msg.left(160));
			out.normalizedArgs = args;
			out.normalizedArgs.insert(QStringLiteral("message"), msg);
			return true;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("send_meeting_chat");
			if (!ctx.meeting || ctx.meeting->state() == MeetingState::Idle) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Meeting session is no longer active.")));
			}
			const QString msg = args.value(QStringLiteral("message")).toString().trimmed();
			if (msg.isEmpty() || msg.size() > kMaxOutboundChatLength) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Invalid chat message.")));
			}
			ctx.meeting->sendChatMessage(msg);
			QJsonObject payload;
			payload.insert(QStringLiteral("sent"), true);
			payload.insert(QStringLiteral("length"), msg.size());
			return makeSimpleOutcome(
				makeOkResult(name, payload),
				QCoreApplication::translate("MeetingUI", "Executed."));
		};
		registerTool(std::move(t));
	}

	// control_collaboration_state
	{
		RegisteredTool t;
		QJsonObject props;
		props.insert(QStringLiteral("action"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Collaboration action: 'stop_screen_share', 'set_screen_share_quality', or 'activate_whiteboard'.")));
		props.insert(QStringLiteral("resolution"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("For 'set_screen_share_quality': 'auto', '720p', '1080p', '1440p', 'native', or '2160p'.")));
		props.insert(QStringLiteral("fps"), makeProp(
			QStringLiteral("integer"),
			QStringLiteral("For 'set_screen_share_quality': frame rate 15, 20, or 30.")));
		t.definition = {
			QStringLiteral("control_collaboration_state"),
			QStringLiteral("Control in-meeting screen sharing or whiteboard collaboration (stop screen share, adjust screen share quality, or open whiteboard). Requires user confirmation."),
			makeObjectSchema(props, {QStringLiteral("action")}),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::ActiveMeeting,
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &,
				PreparedPendingAction &out, QString &outError) {
			const QString action = args.value(QStringLiteral("action")).toString().trimmed().toLower();
			if (action == QStringLiteral("stop_screen_share")) {
				out.title = QCoreApplication::translate("MeetingUI", "Stop Screen Share");
				out.summary = QCoreApplication::translate("MeetingUI", "Stop sharing your screen with the meeting.");
				out.normalizedArgs.insert(QStringLiteral("action"), action);
				return true;
			}
			if (action == QStringLiteral("activate_whiteboard")) {
				out.title = QCoreApplication::translate("MeetingUI", "Open Whiteboard");
				out.summary = QCoreApplication::translate("MeetingUI", "Activate the shared meeting whiteboard.");
				out.normalizedArgs.insert(QStringLiteral("action"), action);
				return true;
			}
			if (action == QStringLiteral("set_screen_share_quality")) {
				const QString resStr = args.value(QStringLiteral("resolution")).toString(QStringLiteral("auto"));
				const auto resOpt = parseScreenShareResolution(resStr);
				if (!resOpt.has_value()) {
					outError = QStringLiteral("Invalid resolution '%1'. Use auto, 720p, 1080p, 1440p, native, or 2160p.")
						.arg(resStr);
					return false;
				}
				const int fps = args.contains(QStringLiteral("fps")) ? args.value(QStringLiteral("fps")).toInt(20) : 20;
				if (fps != 15 && fps != 20 && fps != 30) {
					outError = QStringLiteral("fps must be 15, 20, or 30.");
					return false;
				}
				out.title = QCoreApplication::translate("MeetingUI", "Adjust Screen Share Quality");
				out.summary = QCoreApplication::translate("MeetingUI", "Set screen share quality to %1 at %2 fps.")
					.arg(screenShareResolutionToString(static_cast<int>(*resOpt)))
					.arg(fps);
				out.normalizedArgs.insert(QStringLiteral("action"), action);
				out.normalizedArgs.insert(QStringLiteral("resolution"),
					screenShareResolutionToString(static_cast<int>(*resOpt)));
				out.normalizedArgs.insert(QStringLiteral("fps"), fps);
				return true;
			}
			outError = QStringLiteral("Unsupported collaboration action '%1'.").arg(action);
			return false;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("control_collaboration_state");
			if (!ctx.meeting || ctx.meeting->state() == MeetingState::Idle) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Meeting session is no longer active.")));
			}
			const QString action = args.value(QStringLiteral("action")).toString();
			QJsonObject payload;
			payload.insert(QStringLiteral("action"), action);
			if (action == QStringLiteral("stop_screen_share")) {
				ctx.meeting->stopScreenShare();
			} else if (action == QStringLiteral("activate_whiteboard")) {
				ctx.meeting->activateWhiteboard();
			} else if (action == QStringLiteral("set_screen_share_quality")) {
				const auto resOpt = parseScreenShareResolution(args.value(QStringLiteral("resolution")).toString());
				if (!resOpt.has_value()) {
					return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Invalid screen share resolution.")));
				}
				const int fps = args.value(QStringLiteral("fps")).toInt(20);
				livekit::ScreenShareQuality q;
				q.resolution = *resOpt;
				q.fps = fps;
				ctx.meeting->setScreenShareQuality(q);
				payload.insert(QStringLiteral("resolution"), screenShareResolutionToString(static_cast<int>(*resOpt)));
				payload.insert(QStringLiteral("fps"), fps);
			} else {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Unsupported collaboration action.")));
			}
			return makeSimpleOutcome(
				makeOkResult(name, payload),
				QCoreApplication::translate("MeetingUI", "Executed."));
		};
		registerTool(std::move(t));
	}

	// leave_meeting
	{
		RegisteredTool t;
		QJsonObject leaveProps;
		leaveProps.insert(QStringLiteral("end_for_all"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Optional. True to end the meeting for everyone (host only), false to leave only.")));
		t.definition = {
			QStringLiteral("leave_meeting"),
			QStringLiteral("Leave or end the current meeting (requires user confirmation)."),
			makeObjectSchema(leaveProps),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::ActiveMeeting,
			AgentConcurrencyGroup::MeetingTransition,
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &,
				PreparedPendingAction &out, QString &) {
			const bool endForAll = args.value(QStringLiteral("end_for_all")).toBool(false);
			out.title = endForAll
				? QCoreApplication::translate("MeetingUI", "End Meeting for Everyone")
				: QCoreApplication::translate("MeetingUI", "Leave Meeting");
			out.summary = endForAll
				? QCoreApplication::translate("MeetingUI", "End the active meeting for all participants.")
				: QCoreApplication::translate("MeetingUI", "Disconnect and leave the active meeting.");
			out.normalizedArgs = args;
			return true;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("leave_meeting");
			if (!ctx.meeting || ctx.meeting->state() == MeetingState::Idle) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Meeting session is no longer active.")));
			}
			const bool endForAll = args.value(QStringLiteral("end_for_all")).toBool(false);
			ctx.meeting->leaveMeetingAsync(endForAll);
			QJsonObject payload;
			payload.insert(QStringLiteral("leaving"), true);
			payload.insert(QStringLiteral("endForAll"), endForAll);
			return makeSimpleOutcome(
				makeOkResult(name, payload),
				QCoreApplication::translate("MeetingUI", "Executed."));
		};
		registerTool(std::move(t));
	}
}

void MeetingToolRegistry::registerHostModerationTools() {
	RegisteredTool t;
	QJsonObject props;
	props.insert(QStringLiteral("action"), makeProp(
		QStringLiteral("string"),
		QStringLiteral("Host moderation action: 'mute_all', 'set_participant_mic', 'set_participant_camera', 'transfer_host', 'kick_participant', or 'whiteboard_access'.")));
	props.insert(QStringLiteral("target_user_id"), makeProp(
		QStringLiteral("string"),
		QStringLiteral("Target participant userId (from list_participants) for participant-targeted actions.")));
	props.insert(QStringLiteral("enabled"), makeProp(
		QStringLiteral("boolean"),
		QStringLiteral("For 'set_participant_mic'/'set_participant_camera': true to request unmute/enable, false to mute/disable.")));
	props.insert(QStringLiteral("muted"), makeProp(
		QStringLiteral("boolean"),
		QStringLiteral("For 'mute_all': true to mute all participants (default true), false to request unmute.")));
	props.insert(QStringLiteral("allow_self_unmute"), makeProp(
		QStringLiteral("boolean"),
		QStringLiteral("For 'mute_all': whether participants may unmute themselves (default true).")));
	props.insert(QStringLiteral("reason"), makeProp(
		QStringLiteral("string"),
		QStringLiteral("Optional reason for 'kick_participant'.")));
	props.insert(QStringLiteral("whiteboard_locked"), makeProp(
		QStringLiteral("boolean"),
		QStringLiteral("For 'whiteboard_access': true to lock whiteboard editing, false to unlock.")));
	props.insert(QStringLiteral("whiteboard_writers_open"), makeProp(
		QStringLiteral("boolean"),
		QStringLiteral("For 'whiteboard_access': true to allow participants to draw, false for host-only.")));

	t.definition = {
		QStringLiteral("moderate_participants"),
		QStringLiteral("Host-only meeting moderation: mute all, request participant mic/camera state, transfer host, remove a participant, or manage whiteboard permissions (requires user confirmation)."),
		makeObjectSchema(props, {QStringLiteral("action")}),
		AgentToolRisk::RequiresConfirmation,
		AgentToolScope::MeetingHost,
	};
	t.isAvailable = [](const AgentToolViewContext &ctx) {
		return ctx.meeting && ctx.meeting->state() == MeetingState::InMeeting && ctx.meeting->isHost();
	};
	t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &ctx,
			PreparedPendingAction &out, QString &outError) {
		if (!ctx.meeting || ctx.meeting->state() != MeetingState::InMeeting || !ctx.meeting->isHost()) {
			outError = QStringLiteral("Host permissions in an active meeting are required.");
			return false;
		}
		const QString action = args.value(QStringLiteral("action")).toString().trimmed().toLower();
		out.normalizedArgs.insert(QStringLiteral("action"), action);

		if (action == QStringLiteral("mute_all")) {
			const bool muted = args.contains(QStringLiteral("muted"))
				? args.value(QStringLiteral("muted")).toBool(true)
				: true;
			const bool allowSelf = args.value(QStringLiteral("allow_self_unmute")).toBool(true);
			out.title = muted
				? QCoreApplication::translate("MeetingUI", "Mute All Participants")
				: QCoreApplication::translate("MeetingUI", "Unmute All Participants");
			out.summary = muted
				? QCoreApplication::translate("MeetingUI", "Mute all remote participants in the meeting.")
				: QCoreApplication::translate("MeetingUI", "Request all remote participants to unmute.");
			out.normalizedArgs.insert(QStringLiteral("muted"), muted);
			out.normalizedArgs.insert(QStringLiteral("allow_self_unmute"), allowSelf);
			return true;
		}

		if (action == QStringLiteral("whiteboard_access")) {
			const bool hasLocked = args.contains(QStringLiteral("whiteboard_locked")) &&
				args.value(QStringLiteral("whiteboard_locked")).isBool();
			const bool hasOpen = args.contains(QStringLiteral("whiteboard_writers_open")) &&
				args.value(QStringLiteral("whiteboard_writers_open")).isBool();
			if (!hasLocked && !hasOpen) {
				outError = QStringLiteral("Specify 'whiteboard_locked' or 'whiteboard_writers_open'.");
				return false;
			}
			QStringList parts;
			if (hasLocked) {
				const bool locked = args.value(QStringLiteral("whiteboard_locked")).toBool();
				out.normalizedArgs.insert(QStringLiteral("whiteboard_locked"), locked);
				parts.push_back(locked
					? QCoreApplication::translate("MeetingUI", "Lock whiteboard")
					: QCoreApplication::translate("MeetingUI", "Unlock whiteboard"));
			}
			if (hasOpen) {
				const bool open = args.value(QStringLiteral("whiteboard_writers_open")).toBool();
				out.normalizedArgs.insert(QStringLiteral("whiteboard_writers_open"), open);
				parts.push_back(open
					? QCoreApplication::translate("MeetingUI", "Allow participants to write")
					: QCoreApplication::translate("MeetingUI", "Restrict whiteboard to host"));
			}
			out.title = QCoreApplication::translate("MeetingUI", "Update Whiteboard Permissions");
			out.summary = parts.join(QStringLiteral("\n"));
			return true;
		}

		const QString targetId = args.value(QStringLiteral("target_user_id")).toString().trimmed();
		if (targetId.isEmpty()) {
			outError = QStringLiteral("Missing required parameter 'target_user_id'.");
			return false;
		}
		const QString displayName = resolveParticipantDisplayName(ctx.meeting, targetId);
		out.normalizedArgs.insert(QStringLiteral("target_user_id"), targetId);

		if (action == QStringLiteral("set_participant_mic")) {
			if (!args.value(QStringLiteral("enabled")).isBool()) {
				outError = QStringLiteral("Missing boolean parameter 'enabled'.");
				return false;
			}
			const bool en = args.value(QStringLiteral("enabled")).toBool();
			out.normalizedArgs.insert(QStringLiteral("enabled"), en);
			out.title = en
				? QCoreApplication::translate("MeetingUI", "Request Participant Unmute")
				: QCoreApplication::translate("MeetingUI", "Mute Participant");
			out.summary = en
				? QCoreApplication::translate("MeetingUI", "Ask \"%1\" to turn on their microphone.").arg(displayName)
				: QCoreApplication::translate("MeetingUI", "Mute \"%1\"'s microphone.").arg(displayName);
			return true;
		}

		if (action == QStringLiteral("set_participant_camera")) {
			if (!args.value(QStringLiteral("enabled")).isBool()) {
				outError = QStringLiteral("Missing boolean parameter 'enabled'.");
				return false;
			}
			const bool en = args.value(QStringLiteral("enabled")).toBool();
			out.normalizedArgs.insert(QStringLiteral("enabled"), en);
			out.title = en
				? QCoreApplication::translate("MeetingUI", "Request Participant Camera")
				: QCoreApplication::translate("MeetingUI", "Turn Off Participant Camera");
			out.summary = en
				? QCoreApplication::translate("MeetingUI", "Ask \"%1\" to turn on their camera.").arg(displayName)
				: QCoreApplication::translate("MeetingUI", "Turn off \"%1\"'s camera.").arg(displayName);
			return true;
		}

		if (action == QStringLiteral("transfer_host")) {
			if (targetId == ctx.meeting->meetingDetail().effectiveHostUserId()) {
				outError = QStringLiteral("Selected user is already the host.");
				return false;
			}
			out.title = QCoreApplication::translate("MeetingUI", "Transfer Host Role");
			out.summary = QCoreApplication::translate("MeetingUI", "Transfer meeting host permissions to \"%1\".").arg(displayName);
			return true;
		}

		if (action == QStringLiteral("kick_participant")) {
			if (targetId == ctx.meeting->meetingDetail().effectiveHostUserId()) {
				outError = QStringLiteral("Cannot remove the meeting host.");
				return false;
			}
			const QString reason = args.value(QStringLiteral("reason")).toString().trimmed();
			if (!reason.isEmpty()) {
				out.normalizedArgs.insert(QStringLiteral("reason"), reason.left(128));
			}
			out.title = QCoreApplication::translate("MeetingUI", "Remove Participant");
			out.summary = QCoreApplication::translate("MeetingUI", "Remove \"%1\" from the current meeting.").arg(displayName);
			return true;
		}

		outError = QStringLiteral("Unsupported moderation action '%1'.").arg(action);
		return false;
	};
	t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
		const QString name = QStringLiteral("moderate_participants");
		if (!ctx.meeting || ctx.meeting->state() != MeetingState::InMeeting || !ctx.meeting->isHost()) {
			return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Host permissions in an active meeting are required.")));
		}
		const QString action = args.value(QStringLiteral("action")).toString();
		const QString targetId = args.value(QStringLiteral("target_user_id")).toString();
		QJsonObject payload;
		payload.insert(QStringLiteral("action"), action);

		if (action == QStringLiteral("mute_all")) {
			const bool muted = args.value(QStringLiteral("muted")).toBool(true);
			const bool allowSelf = args.value(QStringLiteral("allow_self_unmute")).toBool(true);
			ctx.meeting->muteAllParticipants(muted, allowSelf);
			payload.insert(QStringLiteral("muted"), muted);
		} else if (action == QStringLiteral("whiteboard_access")) {
			if (args.contains(QStringLiteral("whiteboard_locked"))) {
				ctx.meeting->setWhiteboardLocked(args.value(QStringLiteral("whiteboard_locked")).toBool());
			}
			if (args.contains(QStringLiteral("whiteboard_writers_open"))) {
				ctx.meeting->setWhiteboardWritersOpen(args.value(QStringLiteral("whiteboard_writers_open")).toBool());
			}
		} else if (action == QStringLiteral("set_participant_mic")) {
			const bool en = args.value(QStringLiteral("enabled")).toBool();
			ctx.meeting->requestParticipantMicrophone(targetId, en);
			payload.insert(QStringLiteral("targetUserId"), targetId);
			payload.insert(QStringLiteral("enabled"), en);
		} else if (action == QStringLiteral("set_participant_camera")) {
			const bool en = args.value(QStringLiteral("enabled")).toBool();
			ctx.meeting->requestParticipantCamera(targetId, en);
			payload.insert(QStringLiteral("targetUserId"), targetId);
			payload.insert(QStringLiteral("enabled"), en);
		} else if (action == QStringLiteral("transfer_host")) {
			ctx.meeting->transferHost(targetId);
			payload.insert(QStringLiteral("newHostUserId"), targetId);
		} else if (action == QStringLiteral("kick_participant")) {
			const QString reason = args.value(QStringLiteral("reason")).toString();
			ctx.meeting->kickParticipant(targetId, reason);
			payload.insert(QStringLiteral("removedUserId"), targetId);
		} else {
			return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Unsupported moderation action.")));
		}

		return makeSimpleOutcome(
			makeOkResult(name, payload),
			QCoreApplication::translate("MeetingUI", "Executed."));
	};
	registerTool(std::move(t));
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
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &,
				PreparedPendingAction &out, QString &outError) {
			MeetingBookingRequest req;
			if (!parseBookingRequestFromArgs(args, req, outError)) {
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
			if (!parseBookingRequestFromArgs(args, req, err)) {
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
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &,
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

void MeetingToolRegistry::registerClientSettingsTools() {
	// get_client_preferences (new comprehensive read tool)
	{
		RegisteredTool t;
		t.definition = {
			QStringLiteral("get_client_preferences"),
			QStringLiteral("Read all local client settings (theme, language, audio/video devices, join defaults, 3A audio processing, video encoder backend, camera/screen-share codecs such as H.265/H.264/AV1, mirror mode, resolution/fps preferences, UI display flags, E2EE default switches, and non-secret Copilot settings). Also returns localized 'uiDetails' with exact UI menu paths, current UI labels, and valid localized options."),
			makeObjectSchema({}),
			AgentToolRisk::ReadOnly,
			AgentToolScope::ClientGlobal,
		};
		t.executeReadOnly = [](const QJsonObject &, const AgentToolViewContext &ctx) {
			const QString name = QStringLiteral("get_client_preferences");
			if (!ctx.sessionManager) {
				return makeErrorResult(name, QStringLiteral("Session preferences are unavailable."));
			}
			return makeOkResult(name, serializeClientPreferences(
				*ctx.sessionManager, ctx.agentCoordinator, ctx.hostSettings));
		};
		registerTool(std::move(t));
	}

	// list_media_devices
	{
		RegisteredTool t;
		t.definition = {
			QStringLiteral("list_media_devices"),
			QStringLiteral("List available camera, microphone, and speaker devices along with the currently selected device IDs."),
			makeObjectSchema({}),
			AgentToolRisk::ReadOnly,
			AgentToolScope::ClientGlobal,
		};
		t.executeReadOnly = [](const QJsonObject &, const AgentToolViewContext &ctx) {
			const QString name = QStringLiteral("list_media_devices");
			QJsonArray cameras;
			for (const auto &cam : livekit::CameraEnumerator::EnumerateVideoDevices()) {
				QString id = QString::fromStdString(cam.path);
				if (id.isEmpty()) {
					id = QString::fromStdString(cam.name);
				}
				QJsonObject obj;
				obj.insert(QStringLiteral("id"), id);
				obj.insert(QStringLiteral("name"), QString::fromStdString(cam.name));
				obj.insert(QStringLiteral("isDefault"), cam.is_default);
				cameras.append(obj);
			}
			QJsonArray microphones;
			for (const auto &mic : livekit::AudioDeviceEnumerator::EnumerateInputDevices()) {
				QJsonObject obj;
				obj.insert(QStringLiteral("id"), QString::fromStdString(mic.id));
				obj.insert(QStringLiteral("name"), QString::fromStdString(mic.name));
				obj.insert(QStringLiteral("isDefault"), mic.is_default);
				microphones.append(obj);
			}
			QJsonArray speakers;
			for (const auto &spk : livekit::AudioDeviceEnumerator::EnumerateOutputDevices()) {
				QJsonObject obj;
				obj.insert(QStringLiteral("id"), QString::fromStdString(spk.id));
				obj.insert(QStringLiteral("name"), QString::fromStdString(spk.name));
				obj.insert(QStringLiteral("isDefault"), spk.is_default);
				speakers.append(obj);
			}
			QJsonObject payload;
			payload.insert(QStringLiteral("cameras"), cameras);
			payload.insert(QStringLiteral("microphones"), microphones);
			payload.insert(QStringLiteral("speakers"), speakers);
			if (ctx.sessionManager) {
				const auto &prefs = ctx.sessionManager->mediaPreferences();
				payload.insert(QStringLiteral("selectedCameraDeviceId"), prefs.cameraDeviceId);
				payload.insert(QStringLiteral("selectedMicrophoneDeviceId"), prefs.microphoneDeviceId);
				payload.insert(QStringLiteral("selectedSpeakerDeviceId"), prefs.speakerDeviceId);
			}
			return makeOkResult(name, payload);
		};
		registerTool(std::move(t));
	}

	// get_app_update_status
	{
		RegisteredTool t;
		t.definition = {
			QStringLiteral("get_app_update_status"),
			QStringLiteral("Read the application name, current version, and current software update check/download status (from Settings -> About)."),
			makeObjectSchema({}),
			AgentToolRisk::ReadOnly,
			AgentToolScope::ClientGlobal,
		};
		t.executeReadOnly = [](const QJsonObject &, const AgentToolViewContext &ctx) {
			const QString name = QStringLiteral("get_app_update_status");
			if (!ctx.hostSettings || !ctx.hostSettings->getAppUpdateStatus) {
				return makeErrorResult(name, QStringLiteral("Application update service is unavailable."));
			}
			return makeOkResult(name, ctx.hostSettings->getAppUpdateStatus());
		};
		registerTool(std::move(t));
	}

	// check_for_updates
	{
		RegisteredTool t;
		t.definition = {
			QStringLiteral("check_for_updates"),
			QStringLiteral("Trigger a software update check (equivalent to clicking 'Check for Updates' in Settings -> About) and return the immediate status."),
			makeObjectSchema({}),
			AgentToolRisk::ReadOnly,
			AgentToolScope::ClientGlobal,
		};
		t.executeReadOnly = [](const QJsonObject &, const AgentToolViewContext &ctx) {
			const QString name = QStringLiteral("check_for_updates");
			if (!ctx.hostSettings || !ctx.hostSettings->performAppUpdateAction) {
				return makeErrorResult(name, QStringLiteral("Application update service is unavailable."));
			}
			const QJsonObject res = ctx.hostSettings->performAppUpdateAction(QStringLiteral("check"));
			if (res.contains(QStringLiteral("error")) && !res.value(QStringLiteral("ok")).toBool(true)) {
				return makeErrorResult(name, res.value(QStringLiteral("error")).toString());
			}
			return makeOkResult(name, res);
		};
		registerTool(std::move(t));
	}

	// control_app_update
	{
		RegisteredTool t;
		QJsonObject props;
		props.insert(QStringLiteral("action"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Update action: 'download' (download available update), 'install' (launch verified installer), or 'cancel' (cancel active update check/download).")));
		t.definition = {
			QStringLiteral("control_app_update"),
			QStringLiteral("Download, install, or cancel a client software update (requires user confirmation)."),
			makeObjectSchema(props, {QStringLiteral("action")}),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::ClientGlobal,
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &,
				PreparedPendingAction &out, QString &outError) {
			const QString action = args.value(QStringLiteral("action")).toString().trimmed().toLower();
			if (action == QStringLiteral("download")) {
				out.title = QCoreApplication::translate("MeetingUI", "Download Update");
				out.summary = QCoreApplication::translate("MeetingUI", "Download the available Cohavora update package.");
			} else if (action == QStringLiteral("install")) {
				out.title = QCoreApplication::translate("MeetingUI", "Start Installer");
				out.summary = QCoreApplication::translate("MeetingUI", "Launch the verified installer to update Cohavora.");
			} else if (action == QStringLiteral("cancel")) {
				out.title = QCoreApplication::translate("MeetingUI", "Cancel Update");
				out.summary = QCoreApplication::translate("MeetingUI", "Cancel the ongoing update check or download.");
			} else {
				outError = QStringLiteral("Invalid update action '%1'. Use download, install, or cancel.").arg(action);
				return false;
			}
			out.normalizedArgs.insert(QStringLiteral("action"), action);
			return true;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("control_app_update");
			if (!ctx.hostSettings || !ctx.hostSettings->performAppUpdateAction) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Application update service is unavailable.")));
			}
			const QString action = args.value(QStringLiteral("action")).toString();
			const QJsonObject res = ctx.hostSettings->performAppUpdateAction(action);
			if (!res.value(QStringLiteral("ok")).toBool(true)) {
				return makeSimpleOutcome(makeErrorResult(name, res.value(QStringLiteral("error")).toString(QStringLiteral("Update action failed."))));
			}
			return makeSimpleOutcome(
				makeOkResult(name, res),
				QCoreApplication::translate("MeetingUI", "Executed."));
		};
		registerTool(std::move(t));
	}

	// get_join_media_preferences (hidden legacy alias)
	{
		RegisteredTool t;
		t.definition = {
			QStringLiteral("get_join_media_preferences"),
			QStringLiteral("Read the user's local client Settings -> Audio / Video join preferences."),
			makeObjectSchema({}),
			AgentToolRisk::ReadOnly,
			AgentToolScope::ClientGlobal,
		};
		t.exposedInToolDefinitions = false;
		t.executeReadOnly = [](const QJsonObject &, const AgentToolViewContext &ctx) {
			const QString name = QStringLiteral("get_join_media_preferences");
			if (!ctx.sessionManager) {
				return makeErrorResult(name, QStringLiteral("Session preferences are unavailable."));
			}
			const auto &prefs = ctx.sessionManager->mediaPreferences();
			QJsonObject payload;
			payload.insert(QStringLiteral("enableMicrophoneOnJoin"), prefs.enableMicrophone);
			payload.insert(QStringLiteral("defaultMutedOnJoin"), !prefs.enableMicrophone);
			payload.insert(QStringLiteral("enableCameraOnJoin"), prefs.enableVideo);
			payload.insert(QStringLiteral("enableSpeakerOnJoin"), prefs.enableSpeaker);
			payload.insert(QStringLiteral("pushToTalkWhenMuted"), prefs.pushToTalkWhenMuted);
			return makeOkResult(name, payload);
		};
		registerTool(std::move(t));
	}

	// update_client_preferences (comprehensive settings update tool)
	{
		RegisteredTool t;
		QJsonObject props;
		props.insert(QStringLiteral("enable_microphone_on_join"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Set 'Enable microphone when joining a meeting' (false = muted by default on join).")));
		props.insert(QStringLiteral("enable_camera_on_join"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Set 'Enable camera when joining a meeting'.")));
		props.insert(QStringLiteral("enable_speaker_on_join"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Set 'Use computer audio when joining a meeting'.")));
		props.insert(QStringLiteral("echo_cancellation"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Enable or disable acoustic echo cancellation (AEC).")));
		props.insert(QStringLiteral("noise_suppression"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Enable or disable audio noise suppression (ANS).")));
		props.insert(QStringLiteral("auto_gain_control"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Enable or disable automatic gain control (AGC).")));
		props.insert(QStringLiteral("push_to_talk_when_muted"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Enable or disable push-to-talk (hold Space to unmute temporarily while muted).")));
		props.insert(QStringLiteral("mirror_mode"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Video mirror mode: 'off', 'local_only', or 'local_and_remote'.")));
		props.insert(QStringLiteral("receive_resolution_match"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Remote video resolution matching: 'auto', 'up', 'down', or 'nearest'.")));
		props.insert(QStringLiteral("video_encoder_preference"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Video encoder backend preference (shared by camera and screen share): 'auto' (prefer hardware), 'software', 'nvenc', 'qsv', 'amf' (Windows), or 'vaapi' (Linux).")));
		props.insert(QStringLiteral("camera_video_codec"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Camera video codec: 'auto', 'vp8', 'h264', 'vp9', 'av1', or 'h265' (HEVC).")));
		props.insert(QStringLiteral("screen_share_video_codec"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Screen share video codec: 'auto', 'vp8', 'h264', 'vp9', 'av1', or 'h265' (HEVC).")));
		props.insert(QStringLiteral("video_capture_width"), makeProp(
			QStringLiteral("integer"),
			QStringLiteral("Camera capture width in pixels (0 for automatic ~1080p, or explicit width such as 1280/1920).")));
		props.insert(QStringLiteral("video_capture_height"), makeProp(
			QStringLiteral("integer"),
			QStringLiteral("Camera capture height in pixels (0 for automatic ~1080p, or explicit height such as 720/1080).")));
		props.insert(QStringLiteral("video_capture_fps"), makeProp(
			QStringLiteral("integer"),
			QStringLiteral("Camera capture frame rate (15, 24, 30, or 60).")));
		props.insert(QStringLiteral("screen_share_resolution"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Screen share resolution preset: 'auto', '720p', '1080p', '1440p', 'native', or '2160p'.")));
		props.insert(QStringLiteral("screen_share_fps"), makeProp(
			QStringLiteral("integer"),
			QStringLiteral("Screen share frame rate (15, 20, or 30).")));
		props.insert(QStringLiteral("show_active_speaker"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Highlight the active speaker in meeting layouts.")));
		props.insert(QStringLiteral("participant_name_display"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Participant name overlay display mode: 'auto', 'always', or 'on_hover'.")));
		props.insert(QStringLiteral("stay_in_meeting_when_locked"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Stay in the meeting when the screen is locked.")));
		props.insert(QStringLiteral("quit_on_main_window_close"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Quit application when closing the main window (false minimizes to tray).")));
		props.insert(QStringLiteral("quick_meetings_e2ee"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Enable end-to-end encryption by default for quick meetings.")));
		props.insert(QStringLiteral("screen_share_e2ee"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Enable E2EE by default for screen share.")));
		props.insert(QStringLiteral("meeting_details_e2ee"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Enable E2EE by default for scheduled meeting details.")));
		props.insert(QStringLiteral("all_meetings_e2ee"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Require E2EE for all meetings.")));
		props.insert(QStringLiteral("clear_e2ee_key"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Pass true to clear the in-memory E2EE encryption key.")));
		props.insert(QStringLiteral("theme"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Application UI theme (Settings > General > Theme): pass 'system' (System), 'light' (Light), 'dark' (Dark), or 'teal' (Teal).")));
		props.insert(QStringLiteral("language"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Application UI language (Settings > General > Language, takes effect on next launch): pass 'system' (Follow system), 'zh_CN' (Simplified Chinese), or 'en_US' (English).")));
		props.insert(QStringLiteral("camera_device_id"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Preferred camera device ID (from list_media_devices, or empty string for default).")));
		props.insert(QStringLiteral("microphone_device_id"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Preferred microphone device ID (from list_media_devices, or empty string for default).")));
		props.insert(QStringLiteral("speaker_device_id"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Preferred speaker device ID (from list_media_devices, or empty string for default).")));
		props.insert(QStringLiteral("copilot_profile_name"), makeProp(
			QStringLiteral("string"),
			QStringLiteral("Display name of the Copilot profile.")));
		props.insert(QStringLiteral("copilot_temperature"), makeProp(
			QStringLiteral("number"),
			QStringLiteral("Copilot sampling temperature (0.0 to 2.0).")));
		props.insert(QStringLiteral("copilot_max_context_messages"), makeProp(
			QStringLiteral("integer"),
			QStringLiteral("Maximum history messages sent per Copilot request (2 to 64).")));
		props.insert(QStringLiteral("copilot_send_context_by_default"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Automatically attach meeting context when joining a meeting.")));

		t.definition = {
			QStringLiteral("update_client_preferences"),
			QStringLiteral("Update local client settings (theme, language, audio/video devices, join defaults, 3A audio processing, video encoder backend like nvenc/qsv/amf/software, camera/screen-share codecs like h265/h264/av1, mirror mode, capture/share resolution & fps, UI behavior, E2EE switches, and non-secret Copilot settings). Always refer to user-facing Settings labels (or 'uiDetails' in the tool result) instead of raw enum IDs when replying to the user. Requires user confirmation."),
			makeObjectSchema(props),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::ClientGlobal,
		};
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &ctx,
				PreparedPendingAction &out, QString &outError) {
			QStringList lines;
			const MediaPreferences *curMedia = ctx.sessionManager ? &ctx.sessionManager->mediaPreferences() : nullptr;
			const MeetingSecurityPreferences *curSec = ctx.sessionManager ? &ctx.sessionManager->meetingSecurityPreferences() : nullptr;

			const auto addBool = [&](const QString &key, const QString &label, std::optional<bool> currentVal) {
				if (!args.contains(key)) {
					return true;
				}
				if (!args.value(key).isBool()) {
					outError = QStringLiteral("Parameter '%1' must be a boolean.").arg(key);
					return false;
				}
				const bool target = args.value(key).toBool();
				out.normalizedArgs.insert(key, target);
				if (currentVal.has_value()) {
					lines.push_back(QStringLiteral("%1: %2 -> %3")
						.arg(label, boolDisplayLabel(*currentVal), boolDisplayLabel(target)));
				} else {
					lines.push_back(QStringLiteral("%1 -> %2").arg(label, boolDisplayLabel(target)));
				}
				return true;
			};

			if (!addBool(QStringLiteral("enable_microphone_on_join"),
					QCoreApplication::translate("MeetingUI", "Enable microphone on joining"),
					curMedia ? std::optional<bool>(curMedia->enableMicrophone) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("enable_camera_on_join"),
					QCoreApplication::translate("MeetingUI", "Enable camera on joining"),
					curMedia ? std::optional<bool>(curMedia->enableVideo) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("enable_speaker_on_join"),
					QCoreApplication::translate("MeetingUI", "Use computer audio when joining"),
					curMedia ? std::optional<bool>(curMedia->enableSpeaker) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("echo_cancellation"),
					QCoreApplication::translate("MeetingUI", "Echo cancellation (AEC)"),
					curMedia ? std::optional<bool>(curMedia->echoCancellation) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("noise_suppression"),
					QCoreApplication::translate("MeetingUI", "Noise suppression (ANS)"),
					curMedia ? std::optional<bool>(curMedia->noiseSuppression) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("auto_gain_control"),
					QCoreApplication::translate("MeetingUI", "Automatic gain control (AGC)"),
					curMedia ? std::optional<bool>(curMedia->autoGainControl) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("push_to_talk_when_muted"),
					QCoreApplication::translate("MeetingUI", "Hold Space to temporarily unmute"),
					curMedia ? std::optional<bool>(curMedia->pushToTalkWhenMuted) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("show_active_speaker"),
					QCoreApplication::translate("MeetingUI", "Show the active speaker"),
					curMedia ? std::optional<bool>(curMedia->showActiveSpeaker) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("stay_in_meeting_when_locked"),
					QCoreApplication::translate("MeetingUI", "Stay in the meeting when the screen is locked"),
					curMedia ? std::optional<bool>(curMedia->stayInMeetingWhenLocked) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("quit_on_main_window_close"),
					QCoreApplication::translate("MeetingUI", "Quit when the main window is closed"),
					curMedia ? std::optional<bool>(curMedia->quitOnMainWindowClose) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("quick_meetings_e2ee"),
					QCoreApplication::translate("MeetingUI", "Encrypt quick meetings"),
					curSec ? std::optional<bool>(curSec->quickMeetingsE2ee) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("screen_share_e2ee"),
					QCoreApplication::translate("MeetingUI", "Encrypt screen sharing"),
					curSec ? std::optional<bool>(curSec->screenShareE2ee) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("meeting_details_e2ee"),
					QCoreApplication::translate("MeetingUI", "Encrypt meetings joined from meeting details"),
					curSec ? std::optional<bool>(curSec->meetingDetailsE2ee) : std::nullopt)) return false;
			if (!addBool(QStringLiteral("all_meetings_e2ee"),
					QCoreApplication::translate("MeetingUI", "Encrypt all meetings"),
					curSec ? std::optional<bool>(curSec->allMeetingsE2ee) : std::nullopt)) return false;

			if (args.contains(QStringLiteral("mirror_mode"))) {
				const QString raw = args.value(QStringLiteral("mirror_mode")).toString();
				const auto modeOpt = parseMirrorMode(raw);
				if (!modeOpt.has_value()) {
					outError = QStringLiteral("Invalid mirror_mode '%1'. Use off, local_only, or local_and_remote.").arg(raw);
					return false;
				}
				const QString norm = mirrorModeToString(*modeOpt);
				out.normalizedArgs.insert(QStringLiteral("mirror_mode"), norm);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Mirror Video"),
						formatPreferenceDisplayWithParam(localizedMirrorModeLabel(norm), norm)));
			}

			if (args.contains(QStringLiteral("receive_resolution_match"))) {
				const QString raw = args.value(QStringLiteral("receive_resolution_match")).toString().trimmed().toLower();
				if (raw != QStringLiteral("auto") && raw != QStringLiteral("up") &&
					raw != QStringLiteral("down") && raw != QStringLiteral("nearest")) {
					outError = QStringLiteral("Invalid receive_resolution_match '%1'. Use auto, up, down, or nearest.").arg(raw);
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("receive_resolution_match"), raw);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Simulcast resolution matching"),
						formatPreferenceDisplayWithParam(localizedReceiveResolutionMatchLabel(raw), raw)));
			}

			if (args.contains(QStringLiteral("video_encoder_preference"))) {
				const QString raw = args.value(QStringLiteral("video_encoder_preference")).toString().trimmed();
				const auto sel = NormalizeVideoEncoderPreference(raw.toStdString(), currentEncoderPlatform());
				if (sel.reason == VideoEncoderPreferenceReason::UnsupportedOnPlatform) {
					outError = QStringLiteral("Video encoder '%1' is not supported on this platform.").arg(raw);
					return false;
				}
				if (sel.reason == VideoEncoderPreferenceReason::InvalidValue) {
					outError = QStringLiteral("Invalid video_encoder_preference '%1'. Use auto, software, nvenc, qsv, amf (Windows), or vaapi (Linux).").arg(raw);
					return false;
				}
				const QString norm = QString::fromStdString(sel.value);
				out.normalizedArgs.insert(QStringLiteral("video_encoder_preference"), norm);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Video encoder"),
						formatPreferenceDisplayWithParam(localizedVideoEncoderLabel(norm), norm)));
			}

			if (args.contains(QStringLiteral("camera_video_codec"))) {
				const QString raw = args.value(QStringLiteral("camera_video_codec")).toString();
				const auto codecOpt = parseVideoCodecPreferenceInput(raw);
				if (!codecOpt.has_value()) {
					outError = QStringLiteral("Invalid camera_video_codec '%1'. Use auto, vp8, h264, vp9, av1, or h265.").arg(raw);
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("camera_video_codec"), *codecOpt);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Camera codec"),
						formatPreferenceDisplayWithParam(localizedVideoCodecLabel(*codecOpt), codecOpt->toUpper())));
			}

			if (args.contains(QStringLiteral("screen_share_video_codec"))) {
				const QString raw = args.value(QStringLiteral("screen_share_video_codec")).toString();
				const auto codecOpt = parseVideoCodecPreferenceInput(raw);
				if (!codecOpt.has_value()) {
					outError = QStringLiteral("Invalid screen_share_video_codec '%1'. Use auto, vp8, h264, vp9, av1, or h265.").arg(raw);
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("screen_share_video_codec"), *codecOpt);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Screen share codec"),
						formatPreferenceDisplayWithParam(localizedVideoCodecLabel(*codecOpt), codecOpt->toUpper())));
			}

			if (args.contains(QStringLiteral("video_capture_width")) ||
				args.contains(QStringLiteral("video_capture_height"))) {
				const int w = args.contains(QStringLiteral("video_capture_width"))
					? args.value(QStringLiteral("video_capture_width")).toInt(-1)
					: (curMedia ? curMedia->videoCaptureWidth : 0);
				const int h = args.contains(QStringLiteral("video_capture_height"))
					? args.value(QStringLiteral("video_capture_height")).toInt(-1)
					: (curMedia ? curMedia->videoCaptureHeight : 0);
				if (w < 0 || w > 7680 || h < 0 || h > 4320 || ((w == 0) != (h == 0))) {
					outError = QStringLiteral("video_capture_width and video_capture_height must both be 0 (auto) or valid positive dimensions.");
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("video_capture_width"), w);
				out.normalizedArgs.insert(QStringLiteral("video_capture_height"), h);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "HD Camera Quality"),
						(w == 0 && h == 0)
							? QCoreApplication::translate("MeetingUI", "Auto")
							: QStringLiteral("%1x%2").arg(w).arg(h)));
			}

			if (args.contains(QStringLiteral("video_capture_fps"))) {
				const int fps = args.value(QStringLiteral("video_capture_fps")).toInt(0);
				if (fps != 15 && fps != 24 && fps != 30 && fps != 60) {
					outError = QStringLiteral("video_capture_fps must be 15, 24, 30, or 60.");
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("video_capture_fps"), fps);
				lines.push_back(QStringLiteral("%1 -> %2 fps")
					.arg(QCoreApplication::translate("MeetingUI", "Video capture FPS"))
					.arg(fps));
			}

			if (args.contains(QStringLiteral("screen_share_resolution"))) {
				const QString raw = args.value(QStringLiteral("screen_share_resolution")).toString();
				const auto resOpt = parseScreenShareResolution(raw);
				if (!resOpt.has_value()) {
					outError = QStringLiteral("Invalid screen_share_resolution '%1'.").arg(raw);
					return false;
				}
				const QString norm = screenShareResolutionToString(static_cast<int>(*resOpt));
				out.normalizedArgs.insert(QStringLiteral("screen_share_resolution"), norm);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Screen share resolution"),
						formatPreferenceDisplayWithParam(localizedScreenShareResolutionLabel(norm), norm)));
			}

			if (args.contains(QStringLiteral("screen_share_fps"))) {
				const int fps = args.value(QStringLiteral("screen_share_fps")).toInt(0);
				if (fps != 15 && fps != 20 && fps != 30) {
					outError = QStringLiteral("screen_share_fps must be 15, 20, or 30.");
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("screen_share_fps"), fps);
				lines.push_back(QStringLiteral("%1 -> %2 FPS")
					.arg(QCoreApplication::translate("MeetingUI", "Screen share frame rate"))
					.arg(fps));
			}

			if (args.contains(QStringLiteral("participant_name_display"))) {
				const QString raw = args.value(QStringLiteral("participant_name_display")).toString();
				const auto modeOpt = parseParticipantNameDisplay(raw);
				if (!modeOpt.has_value()) {
					outError = QStringLiteral("Invalid participant_name_display '%1'. Use auto, always, or on_hover.").arg(raw);
					return false;
				}
				const QString norm = participantNameDisplayToString(*modeOpt);
				out.normalizedArgs.insert(QStringLiteral("participant_name_display"), norm);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Participant names in meetings"),
						formatPreferenceDisplayWithParam(localizedParticipantNameDisplayLabel(norm), norm)));
			}

			if (args.contains(QStringLiteral("theme"))) {
				const QString raw = args.value(QStringLiteral("theme")).toString();
				const auto themeOpt = parseThemeInput(raw);
				if (!themeOpt.has_value()) {
					outError = QStringLiteral("Invalid theme '%1'. Use system, light, dark, or teal.").arg(raw);
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("theme"), *themeOpt);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Theme"),
						formatPreferenceDisplayWithParam(localizedThemeLabel(*themeOpt), *themeOpt)));
			}

			if (args.contains(QStringLiteral("language"))) {
				const QString raw = args.value(QStringLiteral("language")).toString();
				const auto langOpt = parseLanguageInput(raw);
				if (!langOpt.has_value()) {
					outError = QStringLiteral("Invalid language '%1'. Use system, zh_CN, or en_US.").arg(raw);
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("language"), *langOpt);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Language"),
						formatPreferenceDisplayWithParam(localizedLanguageLabel(*langOpt), *langOpt)));
			}

			const auto addDeviceId = [&](const QString &key, const QString &label) {
				if (!args.contains(key)) return true;
				if (!args.value(key).isString()) {
					outError = QStringLiteral("Parameter '%1' must be a string.").arg(key);
					return false;
				}
				const QString id = args.value(key).toString().trimmed();
				out.normalizedArgs.insert(key, id);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(label, id.isEmpty() ? QCoreApplication::translate("MeetingUI", "System Default") : id));
				return true;
			};
			if (!addDeviceId(QStringLiteral("camera_device_id"),
					QCoreApplication::translate("MeetingUI", "Camera"))) return false;
			if (!addDeviceId(QStringLiteral("microphone_device_id"),
					QCoreApplication::translate("MeetingUI", "Microphone"))) return false;
			if (!addDeviceId(QStringLiteral("speaker_device_id"),
					QCoreApplication::translate("MeetingUI", "Speaker"))) return false;

			if (args.contains(QStringLiteral("clear_e2ee_key"))) {
				if (!args.value(QStringLiteral("clear_e2ee_key")).isBool()) {
					outError = QStringLiteral("Parameter 'clear_e2ee_key' must be a boolean.");
					return false;
				}
				if (args.value(QStringLiteral("clear_e2ee_key")).toBool()) {
					out.normalizedArgs.insert(QStringLiteral("clear_e2ee_key"), true);
					lines.push_back(QCoreApplication::translate("MeetingUI", "Clear key"));
				}
			}

			if (args.contains(QStringLiteral("copilot_profile_name"))) {
				const QString nameVal = args.value(QStringLiteral("copilot_profile_name")).toString().trimmed();
				if (nameVal.isEmpty()) {
					outError = QStringLiteral("copilot_profile_name must not be empty.");
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("copilot_profile_name"), nameVal.left(64));
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Display Name"), nameVal.left(64)));
			}

			if (args.contains(QStringLiteral("copilot_temperature"))) {
				const double temp = args.value(QStringLiteral("copilot_temperature")).toDouble(-1.0);
				if (!std::isfinite(temp) || temp < 0.0 || temp > 2.0) {
					outError = QStringLiteral("copilot_temperature must be between 0.0 and 2.0.");
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("copilot_temperature"), temp);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Temperature"))
					.arg(temp, 0, 'f', 2));
			}

			if (args.contains(QStringLiteral("copilot_max_context_messages"))) {
				const int maxCtx = args.value(QStringLiteral("copilot_max_context_messages")).toInt(0);
				if (maxCtx < 2 || maxCtx > 64) {
					outError = QStringLiteral("copilot_max_context_messages must be between 2 and 64.");
					return false;
				}
				out.normalizedArgs.insert(QStringLiteral("copilot_max_context_messages"), maxCtx);
				lines.push_back(QStringLiteral("%1 -> %2")
					.arg(QCoreApplication::translate("MeetingUI", "Max Context Messages"))
					.arg(maxCtx));
			}

			if (!addBool(QStringLiteral("copilot_send_context_by_default"),
					QCoreApplication::translate("MeetingUI", "Send meeting context by default"),
					ctx.agentCoordinator ? std::optional<bool>(ctx.agentCoordinator->profile().sendMeetingContextByDefault) : std::nullopt)) return false;

			if (lines.isEmpty()) {
				outError = QStringLiteral("At least one valid client preference field must be specified.");
				return false;
			}
			out.title = QCoreApplication::translate("MeetingUI", "Update Client Settings");
			out.summary = lines.join(QStringLiteral("\n"));
			return true;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("update_client_preferences");
			if (!ctx.sessionManager) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Session preferences are unavailable.")));
			}

			MediaPreferences prefs = ctx.sessionManager->mediaPreferences();
			bool mediaChanged = false;
			const auto applyBool = [&](const QString &key, bool &field) {
				if (args.contains(key) && args.value(key).isBool()) {
					field = args.value(key).toBool();
					mediaChanged = true;
				}
			};
			applyBool(QStringLiteral("enable_microphone_on_join"), prefs.enableMicrophone);
			applyBool(QStringLiteral("enable_camera_on_join"), prefs.enableVideo);
			applyBool(QStringLiteral("enable_speaker_on_join"), prefs.enableSpeaker);
			applyBool(QStringLiteral("echo_cancellation"), prefs.echoCancellation);
			applyBool(QStringLiteral("noise_suppression"), prefs.noiseSuppression);
			applyBool(QStringLiteral("auto_gain_control"), prefs.autoGainControl);
			applyBool(QStringLiteral("push_to_talk_when_muted"), prefs.pushToTalkWhenMuted);
			applyBool(QStringLiteral("show_active_speaker"), prefs.showActiveSpeaker);
			applyBool(QStringLiteral("stay_in_meeting_when_locked"), prefs.stayInMeetingWhenLocked);
			applyBool(QStringLiteral("quit_on_main_window_close"), prefs.quitOnMainWindowClose);

			if (args.contains(QStringLiteral("mirror_mode"))) {
				if (const auto m = parseMirrorMode(args.value(QStringLiteral("mirror_mode")).toString())) {
					prefs.mirrorMode = *m;
					mediaChanged = true;
				}
			}
			if (args.contains(QStringLiteral("receive_resolution_match"))) {
				const QByteArray val = args.value(QStringLiteral("receive_resolution_match")).toString().toLatin1();
				prefs.receiveResolutionMatch = livekit::ParseVideoResolutionMatch(val.constData());
				mediaChanged = true;
			}
			if (args.contains(QStringLiteral("video_encoder_preference"))) {
				prefs.videoEncoderPreference = normalizeVideoEncoderPreference(
					args.value(QStringLiteral("video_encoder_preference")).toString());
				mediaChanged = true;
			}
			if (args.contains(QStringLiteral("camera_video_codec"))) {
				prefs.cameraVideoCodec = normalizeVideoCodecPreference(
					args.value(QStringLiteral("camera_video_codec")).toString());
				mediaChanged = true;
			}
			if (args.contains(QStringLiteral("screen_share_video_codec"))) {
				prefs.screenShareVideoCodec = normalizeVideoCodecPreference(
					args.value(QStringLiteral("screen_share_video_codec")).toString());
				mediaChanged = true;
			}
			if (args.contains(QStringLiteral("video_capture_width")) &&
				args.contains(QStringLiteral("video_capture_height"))) {
				prefs.videoCaptureWidth = args.value(QStringLiteral("video_capture_width")).toInt(prefs.videoCaptureWidth);
				prefs.videoCaptureHeight = args.value(QStringLiteral("video_capture_height")).toInt(prefs.videoCaptureHeight);
				mediaChanged = true;
			}
			if (args.contains(QStringLiteral("video_capture_fps"))) {
				prefs.videoCaptureFps = args.value(QStringLiteral("video_capture_fps")).toInt(prefs.videoCaptureFps);
				mediaChanged = true;
			}
			if (args.contains(QStringLiteral("screen_share_resolution"))) {
				if (const auto r = parseScreenShareResolution(args.value(QStringLiteral("screen_share_resolution")).toString())) {
					prefs.screenShareResolution = static_cast<int>(*r);
					mediaChanged = true;
				}
			}
			if (args.contains(QStringLiteral("screen_share_fps"))) {
				prefs.screenShareFps = args.value(QStringLiteral("screen_share_fps")).toInt(prefs.screenShareFps);
				mediaChanged = true;
			}
			if (args.contains(QStringLiteral("participant_name_display"))) {
				if (const auto d = parseParticipantNameDisplay(args.value(QStringLiteral("participant_name_display")).toString())) {
					prefs.participantNameDisplay = *d;
					mediaChanged = true;
				}
			}
			if (args.contains(QStringLiteral("camera_device_id"))) {
				prefs.cameraDeviceId = args.value(QStringLiteral("camera_device_id")).toString();
				mediaChanged = true;
			}
			if (args.contains(QStringLiteral("microphone_device_id"))) {
				prefs.microphoneDeviceId = args.value(QStringLiteral("microphone_device_id")).toString();
				mediaChanged = true;
			}
			if (args.contains(QStringLiteral("speaker_device_id"))) {
				prefs.speakerDeviceId = args.value(QStringLiteral("speaker_device_id")).toString();
				mediaChanged = true;
			}
			if (mediaChanged) {
				ctx.sessionManager->setMediaPreferences(prefs);
			}

			MeetingSecurityPreferences sec = ctx.sessionManager->meetingSecurityPreferences();
			bool secChanged = false;
			const auto applySecBool = [&](const QString &key, bool &field) {
				if (args.contains(key) && args.value(key).isBool()) {
					field = args.value(key).toBool();
					secChanged = true;
				}
			};
			applySecBool(QStringLiteral("quick_meetings_e2ee"), sec.quickMeetingsE2ee);
			applySecBool(QStringLiteral("screen_share_e2ee"), sec.screenShareE2ee);
			applySecBool(QStringLiteral("meeting_details_e2ee"), sec.meetingDetailsE2ee);
			applySecBool(QStringLiteral("all_meetings_e2ee"), sec.allMeetingsE2ee);
			if (secChanged) {
				ctx.sessionManager->setMeetingSecurityPreferences(sec);
			}
			bool otherChanged = false;
			if (args.value(QStringLiteral("clear_e2ee_key")).toBool(false)) {
				ctx.sessionManager->clearMeetingEncryptionKey();
				otherChanged = true;
			}
			if (args.contains(QStringLiteral("theme")) && ctx.hostSettings && ctx.hostSettings->setTheme) {
				if (ctx.hostSettings->setTheme(args.value(QStringLiteral("theme")).toString())) {
					otherChanged = true;
				}
			}
			if (args.contains(QStringLiteral("language")) && ctx.hostSettings && ctx.hostSettings->setLanguage) {
				if (ctx.hostSettings->setLanguage(args.value(QStringLiteral("language")).toString())) {
					otherChanged = true;
				}
			}
			if (ctx.agentCoordinator && (
					args.contains(QStringLiteral("copilot_profile_name")) ||
					args.contains(QStringLiteral("copilot_temperature")) ||
					args.contains(QStringLiteral("copilot_max_context_messages")) ||
					args.contains(QStringLiteral("copilot_send_context_by_default")))) {
				AgentProfile ap = ctx.agentCoordinator->profile();
				if (args.contains(QStringLiteral("copilot_profile_name"))) {
					ap.name = args.value(QStringLiteral("copilot_profile_name")).toString();
				}
				if (args.contains(QStringLiteral("copilot_temperature"))) {
					ap.temperature = args.value(QStringLiteral("copilot_temperature")).toDouble(ap.temperature);
				}
				if (args.contains(QStringLiteral("copilot_max_context_messages"))) {
					ap.maxContextMessages = args.value(QStringLiteral("copilot_max_context_messages")).toInt(ap.maxContextMessages);
				}
				if (args.contains(QStringLiteral("copilot_send_context_by_default"))) {
					ap.sendMeetingContextByDefault = args.value(QStringLiteral("copilot_send_context_by_default")).toBool(ap.sendMeetingContextByDefault);
				}
				ctx.agentCoordinator->setProfile(ap);
				otherChanged = true;
			}

			if (!mediaChanged && !secChanged && !otherChanged) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("No valid client preference specified.")));
			}

			return makeSimpleOutcome(
				makeOkResult(name, serializeClientPreferences(
					*ctx.sessionManager, ctx.agentCoordinator, ctx.hostSettings)),
				QCoreApplication::translate("MeetingUI", "Settings updated."),
				QCoreApplication::translate("MeetingUI", "Client settings have been updated."));
		};
		registerTool(std::move(t));
	}

	// set_join_media_preferences (hidden legacy alias)
	{
		RegisteredTool t;
		QJsonObject joinPrefProps;
		joinPrefProps.insert(QStringLiteral("enable_microphone_on_join"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Set local client setting 'Enable microphone when joining a meeting'.")));
		joinPrefProps.insert(QStringLiteral("enable_camera_on_join"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Optional: set local client setting 'Enable camera when joining a meeting'.")));
		joinPrefProps.insert(QStringLiteral("enable_speaker_on_join"), makeProp(
			QStringLiteral("boolean"),
			QStringLiteral("Optional: set local client setting 'Use computer audio when joining a meeting'.")));
		t.definition = {
			QStringLiteral("set_join_media_preferences"),
			QStringLiteral("Update the user's local client Settings -> Audio / Video join preferences."),
			makeObjectSchema(joinPrefProps),
			AgentToolRisk::RequiresConfirmation,
			AgentToolScope::ClientGlobal,
		};
		t.exposedInToolDefinitions = false;
		t.prepareAction = [](const QJsonObject &args, const AgentToolViewContext &ctx,
				PreparedPendingAction &out, QString &outError) {
			const bool hasMic = args.contains(QStringLiteral("enable_microphone_on_join")) &&
				args.value(QStringLiteral("enable_microphone_on_join")).isBool();
			const bool hasCam = args.contains(QStringLiteral("enable_camera_on_join")) &&
				args.value(QStringLiteral("enable_camera_on_join")).isBool();
			const bool hasSpk = args.contains(QStringLiteral("enable_speaker_on_join")) &&
				args.value(QStringLiteral("enable_speaker_on_join")).isBool();
			if (!hasMic && !hasCam && !hasSpk) {
				outError = QStringLiteral(
					"At least one boolean preference ('enable_microphone_on_join', 'enable_camera_on_join', "
					"or 'enable_speaker_on_join') must be specified.");
				return false;
			}

			const auto stateLabel = [](bool enabled) {
				return enabled
					? QCoreApplication::translate("MeetingUI", "Enabled")
					: QCoreApplication::translate("MeetingUI", "Disabled (Muted on Join)");
			};

			QStringList lines;
			if (hasMic) {
				const bool targetMic = args.value(QStringLiteral("enable_microphone_on_join")).toBool();
				if (ctx.sessionManager) {
					const bool curMic = ctx.sessionManager->mediaPreferences().enableMicrophone;
					lines.push_back(QCoreApplication::translate(
						"MeetingUI",
						"Enable microphone when joining a meeting: %1 -> %2")
						.arg(stateLabel(curMic), stateLabel(targetMic)));
				} else {
					lines.push_back(QCoreApplication::translate(
						"MeetingUI",
						"Enable microphone when joining a meeting -> %1")
						.arg(stateLabel(targetMic)));
				}
			}
			if (hasCam) {
				const bool targetCam = args.value(QStringLiteral("enable_camera_on_join")).toBool();
				if (ctx.sessionManager) {
					const bool curCam = ctx.sessionManager->mediaPreferences().enableVideo;
					lines.push_back(QCoreApplication::translate(
						"MeetingUI",
						"Enable camera when joining a meeting: %1 -> %2")
						.arg(boolDisplayLabel(curCam), boolDisplayLabel(targetCam)));
				} else {
					lines.push_back(QCoreApplication::translate(
						"MeetingUI",
						"Enable camera when joining a meeting -> %1")
						.arg(boolDisplayLabel(targetCam)));
				}
			}
			if (hasSpk) {
				const bool targetSpk = args.value(QStringLiteral("enable_speaker_on_join")).toBool();
				if (ctx.sessionManager) {
					const bool curSpk = ctx.sessionManager->mediaPreferences().enableSpeaker;
					lines.push_back(QCoreApplication::translate(
						"MeetingUI",
						"Use computer audio when joining a meeting: %1 -> %2")
						.arg(boolDisplayLabel(curSpk), boolDisplayLabel(targetSpk)));
				} else {
					lines.push_back(QCoreApplication::translate(
						"MeetingUI",
						"Use computer audio when joining a meeting -> %1")
						.arg(boolDisplayLabel(targetSpk)));
				}
			}

			out.title = QCoreApplication::translate("MeetingUI", "Update Join Media Settings");
			out.summary = lines.join(QStringLiteral("\n"));
			out.normalizedArgs = args;
			return true;
		};
		t.executeConfirmed = [](const QJsonObject &args, const AgentToolExecuteContext &ctx) {
			const QString name = QStringLiteral("set_join_media_preferences");
			if (!ctx.sessionManager) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("Session preferences are unavailable.")));
			}
			const bool hasMic = args.contains(QStringLiteral("enable_microphone_on_join")) &&
				args.value(QStringLiteral("enable_microphone_on_join")).isBool();
			const bool hasCam = args.contains(QStringLiteral("enable_camera_on_join")) &&
				args.value(QStringLiteral("enable_camera_on_join")).isBool();
			const bool hasSpk = args.contains(QStringLiteral("enable_speaker_on_join")) &&
				args.value(QStringLiteral("enable_speaker_on_join")).isBool();
			if (!hasMic && !hasCam && !hasSpk) {
				return makeSimpleOutcome(makeErrorResult(name, QStringLiteral("No valid join media preference specified.")));
			}
			if (hasMic) {
				ctx.sessionManager->setEnableMicrophone(args.value(QStringLiteral("enable_microphone_on_join")).toBool());
			}
			if (hasCam) {
				ctx.sessionManager->setEnableVideo(args.value(QStringLiteral("enable_camera_on_join")).toBool());
			}
			if (hasSpk) {
				ctx.sessionManager->setEnableSpeaker(args.value(QStringLiteral("enable_speaker_on_join")).toBool());
			}
			const auto &prefs = ctx.sessionManager->mediaPreferences();
			QJsonObject payload;
			payload.insert(QStringLiteral("enableMicrophoneOnJoin"), prefs.enableMicrophone);
			payload.insert(QStringLiteral("defaultMutedOnJoin"), !prefs.enableMicrophone);
			payload.insert(QStringLiteral("enableCameraOnJoin"), prefs.enableVideo);
			payload.insert(QStringLiteral("enableSpeakerOnJoin"), prefs.enableSpeaker);
			return makeSimpleOutcome(
				makeOkResult(name, payload),
				QCoreApplication::translate("MeetingUI", "Settings updated."),
				MeetingToolRegistry::formatJoinMediaPreferencesCompletionSummary(args));
		};
		registerTool(std::move(t));
	}
}

} // namespace OpenMeeting
