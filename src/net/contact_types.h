#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>
#include <vector>

namespace OpenMeeting {

constexpr int kContactBlockedCode = 300001;
constexpr int kContactRequestNotFoundCode = 300002;
constexpr int kContactRequestAlreadyHandledCode = 300003;
constexpr int kContactNotFoundCode = 300004;
constexpr int kDepartmentNotFoundCode = 300005;

enum class PresenceStatus {
    Offline = 0,
    Online = 1,
    Away = 2,
    InMeeting = 3,
};

enum class ContactSourceType {
    OrgColleague = 1,
    PersonalAdd = 2,
};

enum class ContactRequestStatus {
    Pending = 0,
    Accepted = 1,
    Rejected = 2,
};

struct UserPresence {
    QString userId;
    PresenceStatus status = PresenceStatus::Offline;
    QString meetingId;
    qint64 updatedAtMs = 0;

    bool operator==(const UserPresence &other) const {
        return userId == other.userId && status == other.status &&
            meetingId == other.meetingId && updatedAtMs == other.updatedAtMs;
    }
};

struct UserProfile {
    QString userId;
    QString account;
    QString nickname;
    QString employeeNo;
    QString email;
    QString phone;
    QString jobTitle;
    QString signature;
    QString avatarUrl;
    QStringList primaryDeptIds;
    QStringList departmentNames;
    bool inOrgDirectory = false;
    int status = 0;
    qint64 updatedAtMs = 0;

    QString displayName() const {
        const QString nick = nickname.trimmed();
        if (!nick.isEmpty()) return nick;
        const QString acc = account.trimmed();
        if (!acc.isEmpty()) return acc;
        return userId;
    }
};

struct OrgDepartment {
    QString deptId;
    QString deptCode;
    QString parentId;
    QString name;
    QString ancestorPath;
    int sortOrder = 0;
    QString memberVisibility; // ""=inherit, "all", "own_dept"
    qint64 version = 0;
};

struct OrgEmployeeRecord {
    QString recordId;
    QString orgCode;
    QString employeeNo;
    QString jobTitle;
    QStringList deptNames;
    int status = 1; // 1 = active, 2 = removed
    qint64 joinedAtMs = 0;
    qint64 removedAtMs = 0;
    QString removedReason;

    bool isRemoved() const { return status == 2; }
};

struct DeptMember {
    QString deptId;
    bool isLeader = false;
    int sortOrder = 0;
    UserProfile profile;
};

struct ContactRelation {
    QString ownerUserId;
    QString targetUserId;
    QString remark;
    bool isFavorite = false;
    bool isBlocked = false;
    QStringList groupIds;
    ContactSourceType sourceType = ContactSourceType::OrgColleague;
    qint64 createdAtMs = 0;
    qint64 updatedAtMs = 0;
    std::optional<UserProfile> profile;

    QString effectiveDisplayName() const {
        const QString r = remark.trimmed();
        if (!r.isEmpty()) return r;
        if (profile) return profile->displayName();
        return targetUserId;
    }
};

struct ContactGroup {
    QString groupId;
    QString name;
    int sortOrder = 0;
    qint64 updatedAtMs = 0;
};

struct ContactRequest {
    QString requestId;
    QString fromUserId;
    QString toUserId;
    QString greeting;
    ContactRequestStatus status = ContactRequestStatus::Pending;
    qint64 createdAtMs = 0;
    qint64 handledAtMs = 0;
    std::optional<UserProfile> fromProfile;
    std::optional<UserProfile> toProfile;
};

struct UserProfileResponse {
    UserProfile profile;
    std::optional<ContactRelation> relation;
    std::vector<OrgEmployeeRecord> orgHistory;
};

struct DirectorySearchResponse {
    qint64 total = 0;
    std::vector<UserProfile> users;
};

struct OrgTreeResponse {
    qint64 treeVersion = 0;
    bool notModified = false;
    QString memberVisibility;
    std::vector<OrgDepartment> departments;
};

struct DeptMembersResponse {
    QString deptId;
    qint64 total = 0;
    std::vector<DeptMember> members;
    bool restricted = false;
};

struct ContactSnapshotResponse {
    qint64 serverTimeMs = 0;
    std::vector<ContactRelation> contacts;
    std::vector<ContactGroup> groups;
    qint64 pendingCount = 0;
};

struct UpsertContactRelationRequest {
    QString targetUserId;
    std::optional<QString> remark;
    std::optional<bool> isFavorite;
    std::optional<bool> isBlocked;
    std::optional<QStringList> groupIds;
};

struct ManageContactGroupRequest {
    QString action; // "create" | "rename" | "reorder" | "delete"
    QString groupId;
    QString name;
    int sortOrder = 0;
};

struct ManageContactGroupResponse {
    std::optional<ContactGroup> group;
    std::vector<ContactGroup> groups;
};

struct SendContactRequestResponse {
    std::optional<ContactRequest> request;
    bool autoAccepted = false;
    std::optional<ContactRelation> relation;
};

struct HandleContactRequestResponse {
    ContactRequest request;
    std::optional<ContactRelation> relation;
};

struct ContactRequestsResponse {
    std::vector<ContactRequest> requests;
    qint64 pendingCount = 0;
};

PresenceStatus presenceStatusFromWire(const QString &wire);
QString presenceStatusToWire(PresenceStatus status);

UserPresence parseUserPresence(const QJsonObject &obj);
UserProfile parseUserProfile(const QJsonObject &obj);
QJsonObject serializeUserProfile(const UserProfile &profile);
OrgDepartment parseOrgDepartment(const QJsonObject &obj);
QJsonObject serializeOrgDepartment(const OrgDepartment &dept);
DeptMember parseDeptMember(const QJsonObject &obj);
ContactRelation parseContactRelation(const QJsonObject &obj);
QJsonObject serializeContactRelation(const ContactRelation &relation);
ContactGroup parseContactGroup(const QJsonObject &obj);
QJsonObject serializeContactGroup(const ContactGroup &group);
ContactRequest parseContactRequest(const QJsonObject &obj);

UserProfileResponse parseUserProfileResponse(const QJsonObject &obj);
DirectorySearchResponse parseDirectorySearchResponse(const QJsonObject &obj);
OrgTreeResponse parseOrgTreeResponse(const QJsonObject &obj);
DeptMembersResponse parseDeptMembersResponse(const QJsonObject &obj);
ContactSnapshotResponse parseContactSnapshotResponse(const QJsonObject &obj);
ManageContactGroupResponse parseManageContactGroupResponse(const QJsonObject &obj);
SendContactRequestResponse parseSendContactRequestResponse(const QJsonObject &obj);
HandleContactRequestResponse parseHandleContactRequestResponse(const QJsonObject &obj);
ContactRequestsResponse parseContactRequestsResponse(const QJsonObject &obj);

} // namespace OpenMeeting

Q_DECLARE_METATYPE(OpenMeeting::PresenceStatus)
Q_DECLARE_METATYPE(OpenMeeting::UserPresence)
Q_DECLARE_METATYPE(OpenMeeting::UserProfile)
Q_DECLARE_METATYPE(OpenMeeting::ContactRelation)
Q_DECLARE_METATYPE(OpenMeeting::ContactRequest)
