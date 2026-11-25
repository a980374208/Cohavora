#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <optional>
#include <vector>

#include "http_types.h"

namespace OpenMeeting {

enum class MeetingStatus {
    Scheduled,
    InProgress,
    Completed,
    Unknown,
};

enum class MeetingRepeatType {
    None,
    Daily,
    Weekly,
    WeekDay,
    Monthly,
    Custom,
    Unknown,
};

struct MeetingRepeatRule {
    MeetingRepeatType type = MeetingRepeatType::None;
    QString rawType = QStringLiteral("None");
    qint64 endDateSeconds = 0;
    int repeatTimes = 0;
    QString unitType;
    int interval = 0;
    std::vector<int> daysOfWeek;
};

struct MeetingSettings {
    bool canParticipantsEnableCamera = false;
    bool canParticipantsUnmuteMicrophone = false;
    bool canParticipantsShareScreen = false;
    bool disableCameraOnJoin = false;
    bool disableMicrophoneOnJoin = false;
    bool canParticipantJoinMeetingEarly = false;
    bool lockMeeting = false;
    bool audioEncouragement = false;
    bool videoMirroring = false;
};

struct MeetingRecord {
    QString meetingId;
    QString title;
    QString creatorUserId;
    QString creatorNickname;
    QString hostUserId;
    std::vector<QString> coHostUserIds;
    std::vector<QString> inviteeUserIds;
    MeetingStatus status = MeetingStatus::Unknown;
    QString rawStatus;
    qint64 startTimeSeconds = 0;
    qint64 scheduledTimeSeconds = 0;
    qint64 meetingDurationSeconds = 0;
    QString timeZone;
    MeetingSettings settings;
    MeetingRepeatRule repeatRule;
};

struct MeetingCatalogDetail {
    MeetingRecord record;
    QString password;
};

using MeetingList = std::vector<MeetingRecord>;

struct MeetingBookingRequest {
    QString title;
    qint64 scheduledTimeSeconds = 0;
    qint64 meetingDurationSeconds = 0;
    QString password;
    QString timeZone;
    MeetingSettings settings;
    MeetingRepeatRule repeatRule;
    std::vector<QString> inviteeUserIds;
    bool allowCustomRepeatRule = false;
};

struct MeetingUpdateRequest {
    QString meetingId;
    std::optional<QString> title;
    std::optional<qint64> scheduledTimeSeconds;
    std::optional<qint64> meetingDurationSeconds;
    std::optional<QString> password;
    std::optional<QString> timeZone;
    std::optional<bool> canParticipantsEnableCamera;
    std::optional<bool> canParticipantsUnmuteMicrophone;
    std::optional<bool> canParticipantsShareScreen;
    std::optional<bool> disableCameraOnJoin;
    std::optional<bool> disableMicrophoneOnJoin;
    std::optional<bool> canParticipantJoinMeetingEarly;
    std::optional<bool> lockMeeting;
    std::optional<bool> audioEncouragement;
    std::optional<bool> videoMirroring;
    std::optional<MeetingRepeatRule> repeatRule;
    std::optional<std::vector<QString>> inviteeUserIds;
    bool allowCustomRepeatRule = false;
};

QString meetingStatusToWire(MeetingStatus status);
MeetingStatus meetingStatusFromWire(const QString &status);
QString meetingRepeatTypeToWire(MeetingRepeatType type);
MeetingRepeatType meetingRepeatTypeFromWire(const QString &type);
bool isMeetingRepeatTypeSupportedForCreate(MeetingRepeatType type);
bool isMeetingRepeatRuleSupportedForWrite(
    const MeetingRepeatRule &rule,
    QString *errorMessage = nullptr,
    bool allowCustomRepeat = false);

bool parseMeetingCatalogDetail(const QJsonObject &object, MeetingCatalogDetail *detail,
                               QString *errorMessage);
bool parseMeetingList(const QJsonValue &value, MeetingList *meetings, QString *errorMessage);

bool validateMeetingBookingRequest(const MeetingBookingRequest &request, QString *errorMessage);
bool validateMeetingUpdateRequest(const MeetingUpdateRequest &request, QString *errorMessage);
QJsonObject meetingBookingToJson(const MeetingBookingRequest &request, const QString &creatorUserId);
QJsonObject meetingUpdateToJson(const MeetingUpdateRequest &request, const QString &updatingUserId);

constexpr int kMeetingAgentAlreadyOnlineCode = 200005;
constexpr int kMeetingAgentClaimBusyCode = 200006;

struct MeetingAgentTokenRequest {
    QString meetingId;
    QString role = QStringLiteral("translator"); // "transcriber" or "translator"
    QString displayName;
    QString sourceLanguage = QStringLiteral("auto");
    QString targetLanguage;
    bool forceTakeover = false;
};

struct ExistingRoomAgentInfo {
    QString identity;
    QString name;
    QString ownerUserId;
    QString role;
    QString state;
    QString targetLanguage;
};

struct MeetingAgentTokenResponse {
    QString meetingId;
    QString agentIdentity;
    LiveKitAuthInfo liveKit;
    qint64 expiresAtSeconds = 0;
    QString replacedAgentIdentity;
    bool conflict = false;
    ExistingRoomAgentInfo existingAgent;
};

struct RoomAgentStatusResponse {
    QString meetingId;
    bool hasOnlineAgent = false;
    std::vector<ExistingRoomAgentInfo> agents;
};

} // namespace OpenMeeting
