#include <QtCore/QCoreApplication>
#include "src/net/meeting_types.h"

#include <QtCore/QJsonArray>
#include <QtCore/QTimeZone>

#include <cmath>
#include <limits>

namespace OpenMeeting {
namespace {

constexpr qint64 kMaxSafeJsonInteger = 9007199254740991LL;

bool fail(QString *errorMessage, const QString &message) {
    if (errorMessage) *errorMessage = message;
    return false;
}

bool parseInt64(const QJsonValue &value, qint64 defaultValue, qint64 *result,
                QString *errorMessage, const QString &field) {
    if (value.isUndefined() || value.isNull()) {
        *result = defaultValue;
        return true;
    }
    if (value.isString()) {
        const auto text = value.toString();
        bool ok = false;
        const auto parsed = text.toLongLong(&ok, 10);
        if (!ok || text.isEmpty() || text.startsWith(QLatin1Char('+')) ||
            (text.size() > 1 && text.startsWith(QLatin1Char('0'))) ||
            (text.size() > 2 && text.startsWith(QStringLiteral("-0")))) {
            return fail(errorMessage, QCoreApplication::translate("MeetingUI", "%1 is not a decimal integer.").arg(field));
        }
        *result = parsed;
        return true;
    }
    if (!value.isDouble()) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "%1 is not an integer.").arg(field));
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number ||
        std::abs(number) > static_cast<double>(kMaxSafeJsonInteger)) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "%1 is outside the safe JSON integer range.").arg(field));
    }
    *result = static_cast<qint64>(number);
    return true;
}

bool parseInt32(const QJsonValue &value, int defaultValue, int *result,
                QString *errorMessage, const QString &field) {
    qint64 parsed = 0;
    if (!parseInt64(value, defaultValue, &parsed, errorMessage, field)) return false;
    if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "%1 is outside the int32 range.").arg(field));
    }
    *result = static_cast<int>(parsed);
    return true;
}

bool parseString(const QJsonValue &value, QString *result, QString *errorMessage,
                 const QString &field, bool required = false) {
    if (value.isUndefined() || value.isNull()) {
        result->clear();
        return required ? fail(errorMessage, QCoreApplication::translate("MeetingUI", "%1 is missing.").arg(field)) : true;
    }
    if (!value.isString()) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "%1 is not a string.").arg(field));
    }
    *result = value.toString();
    if (required && result->isEmpty()) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "%1 is empty.").arg(field));
    }
    return true;
}

bool parseBool(const QJsonObject &object, const char *name, bool *result,
               QString *errorMessage) {
    const auto value = object.value(QLatin1String(name));
    if (value.isUndefined() || value.isNull()) {
        *result = false;
        return true;
    }
    if (!value.isBool()) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "%1 is not a boolean.").arg(QString::fromLatin1(name)));
    }
    *result = value.toBool();
    return true;
}

bool parseSettings(const QJsonValue &value, MeetingSettings *settings, QString *errorMessage) {
    if (value.isUndefined() || value.isNull()) return true;
    if (!value.isObject()) return fail(errorMessage, QCoreApplication::translate("MeetingUI", "setting is not an object."));
    const auto object = value.toObject();
    return parseBool(object, "canParticipantsEnableCamera", &settings->canParticipantsEnableCamera, errorMessage) &&
        parseBool(object, "canParticipantsUnmuteMicrophone", &settings->canParticipantsUnmuteMicrophone, errorMessage) &&
        parseBool(object, "canParticipantsShareScreen", &settings->canParticipantsShareScreen, errorMessage) &&
        parseBool(object, "disableCameraOnJoin", &settings->disableCameraOnJoin, errorMessage) &&
        parseBool(object, "disableMicrophoneOnJoin", &settings->disableMicrophoneOnJoin, errorMessage) &&
        parseBool(object, "canParticipantJoinMeetingEarly", &settings->canParticipantJoinMeetingEarly, errorMessage) &&
        parseBool(object, "lockMeeting", &settings->lockMeeting, errorMessage) &&
        parseBool(object, "audioEncouragement", &settings->audioEncouragement, errorMessage) &&
        parseBool(object, "videoMirroring", &settings->videoMirroring, errorMessage);
}

bool parseRepeatRule(const QJsonValue &value, MeetingRepeatRule *rule, QString *errorMessage) {
    if (value.isUndefined() || value.isNull()) return true;
    if (!value.isObject()) return fail(errorMessage, QCoreApplication::translate("MeetingUI", "repeatInfo is not an object."));
    const auto object = value.toObject();
    if (!parseString(object.value(QStringLiteral("repeatType")), &rule->rawType, errorMessage,
                     QStringLiteral("repeatInfo.repeatType"))) return false;
    if (rule->rawType.isEmpty()) rule->rawType = QStringLiteral("None");
    rule->type = meetingRepeatTypeFromWire(rule->rawType);
    if (!parseInt64(object.value(QStringLiteral("endDate")), 0, &rule->endDateSeconds,
                    errorMessage, QStringLiteral("repeatInfo.endDate")) ||
        !parseInt32(object.value(QStringLiteral("repeatTimes")), 0, &rule->repeatTimes,
                    errorMessage, QStringLiteral("repeatInfo.repeatTimes")) ||
        !parseString(object.value(QStringLiteral("uintType")), &rule->unitType, errorMessage,
                     QStringLiteral("repeatInfo.uintType")) ||
        !parseInt32(object.value(QStringLiteral("interval")), 0, &rule->interval,
                    errorMessage, QStringLiteral("repeatInfo.interval"))) return false;
    const auto days = object.value(QStringLiteral("repeatDaysOfWeek"));
    if (!days.isUndefined() && !days.isNull()) {
        if (!days.isArray()) return fail(errorMessage, QCoreApplication::translate("MeetingUI", "repeatInfo.repeatDaysOfWeek is not an array."));
        for (const auto &dayValue : days.toArray()) {
            int day = 0;
            if (!parseInt32(dayValue, 0, &day, errorMessage,
                            QStringLiteral("repeatInfo.repeatDaysOfWeek")) || day < 0 || day > 6) {
                return fail(errorMessage, QCoreApplication::translate("MeetingUI", "repeatInfo.repeatDaysOfWeek contains an invalid day."));
            }
            rule->daysOfWeek.push_back(day);
        }
    }
    return true;
}

QJsonObject settingsToJson(const MeetingSettings &settings) {
    return {
        {QStringLiteral("canParticipantsEnableCamera"), settings.canParticipantsEnableCamera},
        {QStringLiteral("canParticipantsUnmuteMicrophone"), settings.canParticipantsUnmuteMicrophone},
        {QStringLiteral("canParticipantsShareScreen"), settings.canParticipantsShareScreen},
        {QStringLiteral("disableCameraOnJoin"), settings.disableCameraOnJoin},
        {QStringLiteral("disableMicrophoneOnJoin"), settings.disableMicrophoneOnJoin},
        {QStringLiteral("canParticipantJoinMeetingEarly"), settings.canParticipantJoinMeetingEarly},
        {QStringLiteral("lockMeeting"), settings.lockMeeting},
        {QStringLiteral("audioEncouragement"), settings.audioEncouragement},
        {QStringLiteral("videoMirroring"), settings.videoMirroring},
    };
}

QJsonObject repeatRuleToJson(const MeetingRepeatRule &rule) {
    QJsonArray days;
    for (const int day : rule.daysOfWeek) days.append(day);
    return {
        {QStringLiteral("endDate"), static_cast<double>(rule.endDateSeconds)},
        {QStringLiteral("repeatTimes"), rule.repeatTimes},
        {QStringLiteral("repeatType"), meetingRepeatTypeToWire(rule.type)},
        {QStringLiteral("uintType"), rule.unitType},
        {QStringLiteral("interval"), rule.interval},
        {QStringLiteral("repeatDaysOfWeek"), days},
    };
}

bool safeRequestInteger(qint64 value) {
    return value >= 0 && value <= kMaxSafeJsonInteger;
}

template <typename T>
void addOptional(QJsonObject *object, const QString &name, const std::optional<T> &value) {
    if (value) object->insert(name, QJsonValue(*value));
}

void addOptionalInt64(QJsonObject *object, const QString &name, const std::optional<qint64> &value) {
    if (value) object->insert(name, static_cast<double>(*value));
}

} // namespace

QString meetingStatusToWire(MeetingStatus status) {
    switch (status) {
    case MeetingStatus::Scheduled: return QStringLiteral("Scheduled");
    case MeetingStatus::InProgress: return QStringLiteral("In-Progress");
    case MeetingStatus::Completed: return QStringLiteral("Completed");
    case MeetingStatus::Unknown: return {};
    }
    return {};
}

MeetingStatus meetingStatusFromWire(const QString &status) {
    if (status == QStringLiteral("Scheduled")) return MeetingStatus::Scheduled;
    if (status == QStringLiteral("In-Progress")) return MeetingStatus::InProgress;
    if (status == QStringLiteral("Completed")) return MeetingStatus::Completed;
    return MeetingStatus::Unknown;
}

QString meetingRepeatTypeToWire(MeetingRepeatType type) {
    switch (type) {
    case MeetingRepeatType::None: return QStringLiteral("None");
    case MeetingRepeatType::Daily: return QStringLiteral("Daily");
    case MeetingRepeatType::Weekly: return QStringLiteral("Weekly");
    case MeetingRepeatType::WeekDay: return QStringLiteral("WeekDay");
    case MeetingRepeatType::Monthly: return QStringLiteral("Monthly");
    case MeetingRepeatType::Custom: return QStringLiteral("Custom");
    case MeetingRepeatType::Unknown: return {};
    }
    return {};
}

MeetingRepeatType meetingRepeatTypeFromWire(const QString &type) {
    if (type.isEmpty() || type == QStringLiteral("None")) return MeetingRepeatType::None;
    if (type == QStringLiteral("Daily")) return MeetingRepeatType::Daily;
    if (type == QStringLiteral("Weekly")) return MeetingRepeatType::Weekly;
    if (type == QStringLiteral("WeekDay")) return MeetingRepeatType::WeekDay;
    if (type == QStringLiteral("Monthly")) return MeetingRepeatType::Monthly;
    if (type == QStringLiteral("Custom")) return MeetingRepeatType::Custom;
    return MeetingRepeatType::Unknown;
}

bool isMeetingRepeatTypeSupportedForCreate(MeetingRepeatType type) {
    switch (type) {
    case MeetingRepeatType::None:
    case MeetingRepeatType::Daily:
    case MeetingRepeatType::Weekly:
    case MeetingRepeatType::WeekDay:
    case MeetingRepeatType::Monthly:
        return true;
    case MeetingRepeatType::Custom:
    case MeetingRepeatType::Unknown:
        return false;
    }
    return false;
}

bool isMeetingRepeatRuleSupportedForWrite(
        const MeetingRepeatRule &rule, QString *errorMessage) {
    if (!isMeetingRepeatTypeSupportedForCreate(rule.type)) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "Repeat type is read-only and cannot be submitted."));
    }
    if (!safeRequestInteger(rule.endDateSeconds)) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "Repeat end date is invalid."));
    }
    if (rule.repeatTimes != 0) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "Repeat count is not supported."));
    }
    if (!rule.unitType.trimmed().isEmpty() || rule.interval != 0 || !rule.daysOfWeek.empty()) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "Fixed repeat type contains custom repeat fields."));
    }
    if (rule.type == MeetingRepeatType::None && rule.endDateSeconds != 0) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "A non-repeating meeting cannot have a repeat end date."));
    }
    return true;
}

bool parseMeetingCatalogDetail(
        const QJsonObject &object, MeetingCatalogDetail *detail, QString *errorMessage) {
    if (!detail) return fail(errorMessage, QCoreApplication::translate("MeetingUI", "Meeting output is null."));
    const auto infoValue = object.value(QStringLiteral("info"));
    if (!infoValue.isObject()) return fail(errorMessage, QCoreApplication::translate("MeetingUI", "Meeting info is missing."));
    const auto info = infoValue.toObject();
    const auto systemValue = info.value(QStringLiteral("systemGenerated"));
    const auto creatorValue = info.value(QStringLiteral("creatorDefinedMeeting"));
    if (!systemValue.isObject() || !creatorValue.isObject()) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "Meeting info structure is invalid."));
    }
    const auto system = systemValue.toObject();
    const auto creator = creatorValue.toObject();
    MeetingCatalogDetail parsed;
    auto &record = parsed.record;
    if (!parseString(system.value(QStringLiteral("meetingID")), &record.meetingId, errorMessage,
                     QStringLiteral("info.systemGenerated.meetingID"), true) ||
        !parseString(system.value(QStringLiteral("creatorUserID")), &record.creatorUserId, errorMessage,
                     QStringLiteral("info.systemGenerated.creatorUserID")) ||
        !parseString(system.value(QStringLiteral("creatorNickname")), &record.creatorNickname, errorMessage,
                     QStringLiteral("info.systemGenerated.creatorNickname")) ||
        !parseString(system.value(QStringLiteral("status")), &record.rawStatus, errorMessage,
                     QStringLiteral("info.systemGenerated.status")) ||
        !parseInt64(system.value(QStringLiteral("startTime")), 0, &record.startTimeSeconds,
                    errorMessage, QStringLiteral("info.systemGenerated.startTime")) ||
        !parseString(creator.value(QStringLiteral("title")), &record.title, errorMessage,
                     QStringLiteral("info.creatorDefinedMeeting.title")) ||
        !parseInt64(creator.value(QStringLiteral("scheduledTime")), 0, &record.scheduledTimeSeconds,
                    errorMessage, QStringLiteral("info.creatorDefinedMeeting.scheduledTime")) ||
        !parseInt64(creator.value(QStringLiteral("meetingDuration")), 0, &record.meetingDurationSeconds,
                    errorMessage, QStringLiteral("info.creatorDefinedMeeting.meetingDuration")) ||
        !parseString(creator.value(QStringLiteral("password")), &parsed.password, errorMessage,
                     QStringLiteral("info.creatorDefinedMeeting.password")) ||
        !parseString(creator.value(QStringLiteral("timeZone")), &record.timeZone, errorMessage,
                     QStringLiteral("info.creatorDefinedMeeting.timeZone")) ||
        !parseString(creator.value(QStringLiteral("hostUserID")), &record.hostUserId, errorMessage,
                     QStringLiteral("info.creatorDefinedMeeting.hostUserID"))) return false;
    record.status = meetingStatusFromWire(record.rawStatus);
    const auto coHosts = creator.value(QStringLiteral("coHostUSerID"));
    if (!coHosts.isUndefined() && !coHosts.isNull()) {
        if (!coHosts.isArray()) return fail(errorMessage, QCoreApplication::translate("MeetingUI", "coHostUSerID is not an array."));
        for (const auto &value : coHosts.toArray()) {
            QString id;
            if (!parseString(value, &id, errorMessage, QStringLiteral("coHostUSerID"))) return false;
            record.coHostUserIds.push_back(id);
        }
    }
    if (!parseSettings(object.value(QStringLiteral("setting")), &record.settings, errorMessage) ||
        !parseRepeatRule(object.value(QStringLiteral("repeatInfo")), &record.repeatRule, errorMessage)) return false;
    *detail = std::move(parsed);
    return true;
}

bool parseMeetingList(const QJsonValue &value, MeetingList *meetings, QString *errorMessage) {
    if (!meetings) return fail(errorMessage, QCoreApplication::translate("MeetingUI", "Meeting list output is null."));
    if (value.isUndefined() || value.isNull()) {
        meetings->clear();
        return true;
    }
    if (!value.isArray()) return fail(errorMessage, QCoreApplication::translate("MeetingUI", "meetingDetails is not an array."));
    MeetingList parsed;
    parsed.reserve(static_cast<size_t>(value.toArray().size()));
    for (const auto &item : value.toArray()) {
        if (!item.isObject()) return fail(errorMessage, QCoreApplication::translate("MeetingUI", "meetingDetails contains a non-object item."));
        MeetingCatalogDetail detail;
        if (!parseMeetingCatalogDetail(item.toObject(), &detail, errorMessage)) return false;
        parsed.push_back(std::move(detail.record));
    }
    *meetings = std::move(parsed);
    return true;
}

bool validateMeetingBookingRequest(const MeetingBookingRequest &request, QString *errorMessage) {
    if (request.title.trimmed().isEmpty()) return fail(errorMessage, QCoreApplication::translate("MeetingUI", "Meeting title is empty."));
    if (!safeRequestInteger(request.scheduledTimeSeconds) || request.scheduledTimeSeconds == 0) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "Scheduled time is invalid."));
    }
    if (!safeRequestInteger(request.meetingDurationSeconds) || request.meetingDurationSeconds == 0) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "Meeting duration is invalid."));
    }
    if (request.timeZone.isEmpty() || !QTimeZone(request.timeZone.toUtf8()).isValid()) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "Meeting time zone is invalid."));
    }
    if (!isMeetingRepeatRuleSupportedForWrite(request.repeatRule, errorMessage)) return false;
    if (request.repeatRule.type != MeetingRepeatType::None &&
        request.repeatRule.endDateSeconds != 0 &&
        request.repeatRule.endDateSeconds < request.scheduledTimeSeconds) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "Repeat end date precedes the first meeting."));
    }
    return true;
}

bool validateMeetingUpdateRequest(const MeetingUpdateRequest &request, QString *errorMessage) {
    if (request.meetingId.isEmpty()) return fail(errorMessage, QCoreApplication::translate("MeetingUI", "Meeting ID is empty."));
    const bool hasChange = request.title || request.scheduledTimeSeconds || request.meetingDurationSeconds ||
        request.password || request.timeZone || request.canParticipantsEnableCamera ||
        request.canParticipantsUnmuteMicrophone || request.canParticipantsShareScreen ||
        request.disableCameraOnJoin || request.disableMicrophoneOnJoin ||
        request.canParticipantJoinMeetingEarly || request.lockMeeting || request.audioEncouragement ||
        request.videoMirroring || request.repeatRule;
    if (!hasChange) return fail(errorMessage, QCoreApplication::translate("MeetingUI", "Meeting update contains no changes."));
    if (request.title && request.title->trimmed().isEmpty()) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "Meeting title is empty."));
    }
    if (request.scheduledTimeSeconds &&
        (!safeRequestInteger(*request.scheduledTimeSeconds) || *request.scheduledTimeSeconds == 0)) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "Scheduled time is invalid."));
    }
    if (request.meetingDurationSeconds &&
        (!safeRequestInteger(*request.meetingDurationSeconds) || *request.meetingDurationSeconds == 0)) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "Meeting duration is invalid."));
    }
    if (request.timeZone && (request.timeZone->isEmpty() || !QTimeZone(request.timeZone->toUtf8()).isValid())) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "Meeting time zone is invalid."));
    }
    if (request.repeatRule &&
        !isMeetingRepeatRuleSupportedForWrite(*request.repeatRule, errorMessage)) return false;
    if (request.scheduledTimeSeconds && request.repeatRule->type != MeetingRepeatType::None &&
        request.repeatRule->endDateSeconds != 0 &&
        request.repeatRule->endDateSeconds < *request.scheduledTimeSeconds) {
        return fail(errorMessage, QCoreApplication::translate("MeetingUI", "Repeat end date precedes the first meeting."));
    }
    return true;
}

QJsonObject meetingBookingToJson(const MeetingBookingRequest &request, const QString &creatorUserId) {
    QJsonObject creatorInfo{
        {QStringLiteral("title"), request.title},
        {QStringLiteral("scheduledTime"), static_cast<double>(request.scheduledTimeSeconds)},
        {QStringLiteral("meetingDuration"), static_cast<double>(request.meetingDurationSeconds)},
        {QStringLiteral("password"), request.password},
        {QStringLiteral("timeZone"), request.timeZone},
    };
    return {
        {QStringLiteral("creatorUserID"), creatorUserId},
        {QStringLiteral("creatorDefinedMeetingInfo"), creatorInfo},
        {QStringLiteral("setting"), settingsToJson(request.settings)},
        {QStringLiteral("repeatInfo"), repeatRuleToJson(request.repeatRule)},
    };
}

QJsonObject meetingUpdateToJson(const MeetingUpdateRequest &request, const QString &updatingUserId) {
    QJsonObject object{
        {QStringLiteral("meetingID"), request.meetingId},
        {QStringLiteral("updatingUserID"), updatingUserId},
    };
    addOptional(&object, QStringLiteral("title"), request.title);
    addOptionalInt64(&object, QStringLiteral("scheduledTime"), request.scheduledTimeSeconds);
    addOptionalInt64(&object, QStringLiteral("meetingDuration"), request.meetingDurationSeconds);
    addOptional(&object, QStringLiteral("password"), request.password);
    addOptional(&object, QStringLiteral("timeZone"), request.timeZone);
    addOptional(&object, QStringLiteral("canParticipantsEnableCamera"), request.canParticipantsEnableCamera);
    addOptional(&object, QStringLiteral("canParticipantsUnmuteMicrophone"), request.canParticipantsUnmuteMicrophone);
    addOptional(&object, QStringLiteral("canParticipantsShareScreen"), request.canParticipantsShareScreen);
    addOptional(&object, QStringLiteral("disableCameraOnJoin"), request.disableCameraOnJoin);
    addOptional(&object, QStringLiteral("disableMicrophoneOnJoin"), request.disableMicrophoneOnJoin);
    addOptional(&object, QStringLiteral("canParticipantJoinMeetingEarly"), request.canParticipantJoinMeetingEarly);
    addOptional(&object, QStringLiteral("lockMeeting"), request.lockMeeting);
    addOptional(&object, QStringLiteral("audioEncouragement"), request.audioEncouragement);
    addOptional(&object, QStringLiteral("videoMirroring"), request.videoMirroring);
    if (request.repeatRule) object.insert(QStringLiteral("repeatInfo"), repeatRuleToJson(*request.repeatRule));
    return object;
}

} // namespace OpenMeeting
