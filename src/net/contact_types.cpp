#include "src/net/contact_types.h"

#include <QtCore/QJsonArray>

namespace OpenMeeting {
namespace {

qint64 readInt64(const QJsonObject &obj, const QString &key, qint64 fallback = 0) {
    const QJsonValue v = obj.value(key);
    if (v.isUndefined() || v.isNull()) return fallback;
    if (v.isString()) {
        bool ok = false;
        const qint64 parsed = v.toString().toLongLong(&ok);
        return ok ? parsed : fallback;
    }
    return static_cast<qint64>(v.toDouble(static_cast<double>(fallback)));
}

QStringList readStringList(const QJsonValue &val) {
    QStringList out;
    if (!val.isArray()) return out;
    for (const auto &item : val.toArray()) {
        const QString s = item.toString().trimmed();
        if (!s.isEmpty()) out.push_back(s);
    }
    return out;
}

QJsonArray toJsonArray(const QStringList &list) {
    QJsonArray arr;
    for (const auto &s : list) arr.append(s);
    return arr;
}

} // namespace

PresenceStatus presenceStatusFromWire(const QString &wire) {
    const QString lowered = wire.trimmed().toLower();
    if (lowered == QStringLiteral("in_meeting")) return PresenceStatus::InMeeting;
    if (lowered == QStringLiteral("online")) return PresenceStatus::Online;
    if (lowered == QStringLiteral("away")) return PresenceStatus::Away;
    return PresenceStatus::Offline;
}

QString presenceStatusToWire(PresenceStatus status) {
    switch (status) {
    case PresenceStatus::Online: return QStringLiteral("online");
    case PresenceStatus::Away: return QStringLiteral("away");
    case PresenceStatus::InMeeting: return QStringLiteral("in_meeting");
    case PresenceStatus::Offline:
    default: return QStringLiteral("offline");
    }
}

UserPresence parseUserPresence(const QJsonObject &obj) {
    UserPresence rec;
    rec.userId = obj.value(QStringLiteral("userID")).toString();
    rec.status = presenceStatusFromWire(obj.value(QStringLiteral("status")).toString());
    rec.meetingId = obj.value(QStringLiteral("meetingID")).toString();
    rec.updatedAtMs = readInt64(obj, QStringLiteral("updatedAtMs"));
    return rec;
}

UserProfile parseUserProfile(const QJsonObject &obj) {
    UserProfile p;
    p.userId = obj.value(QStringLiteral("userID")).toString();
    p.account = obj.value(QStringLiteral("account")).toString();
    p.nickname = obj.value(QStringLiteral("nickname")).toString();
    p.employeeNo = obj.value(QStringLiteral("employeeNo")).toString();
    p.email = obj.value(QStringLiteral("email")).toString();
    p.phone = obj.value(QStringLiteral("phone")).toString();
    p.jobTitle = obj.value(QStringLiteral("jobTitle")).toString();
    p.signature = obj.value(QStringLiteral("signature")).toString();
    p.avatarUrl = obj.value(QStringLiteral("avatarURL")).toString();
    p.primaryDeptIds = readStringList(obj.value(QStringLiteral("primaryDeptIDs")));
    p.departmentNames = readStringList(obj.value(QStringLiteral("departmentNames")));
    p.inOrgDirectory = obj.value(QStringLiteral("inOrgDirectory")).toBool(!p.primaryDeptIds.isEmpty());
    p.status = obj.value(QStringLiteral("status")).toInt(0);
    p.updatedAtMs = readInt64(obj, QStringLiteral("updatedAtMs"));
    return p;
}

QJsonObject serializeUserProfile(const UserProfile &profile) {
    QJsonObject obj;
    obj.insert(QStringLiteral("userID"), profile.userId);
    obj.insert(QStringLiteral("account"), profile.account);
    obj.insert(QStringLiteral("nickname"), profile.nickname);
    if (!profile.employeeNo.isEmpty()) obj.insert(QStringLiteral("employeeNo"), profile.employeeNo);
    if (!profile.email.isEmpty()) obj.insert(QStringLiteral("email"), profile.email);
    if (!profile.phone.isEmpty()) obj.insert(QStringLiteral("phone"), profile.phone);
    if (!profile.jobTitle.isEmpty()) obj.insert(QStringLiteral("jobTitle"), profile.jobTitle);
    if (!profile.signature.isEmpty()) obj.insert(QStringLiteral("signature"), profile.signature);
    if (!profile.avatarUrl.isEmpty()) obj.insert(QStringLiteral("avatarURL"), profile.avatarUrl);
    obj.insert(QStringLiteral("primaryDeptIDs"), toJsonArray(profile.primaryDeptIds));
    obj.insert(QStringLiteral("departmentNames"), toJsonArray(profile.departmentNames));
    obj.insert(QStringLiteral("inOrgDirectory"), profile.inOrgDirectory);
    obj.insert(QStringLiteral("status"), profile.status);
    obj.insert(QStringLiteral("updatedAtMs"), static_cast<double>(profile.updatedAtMs));
    return obj;
}

OrgDepartment parseOrgDepartment(const QJsonObject &obj) {
    OrgDepartment d;
    d.deptId = obj.value(QStringLiteral("deptID")).toString();
    d.deptCode = obj.value(QStringLiteral("deptCode")).toString();
    d.parentId = obj.value(QStringLiteral("parentID")).toString();
    d.name = obj.value(QStringLiteral("name")).toString();
    d.ancestorPath = obj.value(QStringLiteral("ancestorPath")).toString();
    d.sortOrder = obj.value(QStringLiteral("sortOrder")).toInt(0);
    d.memberVisibility = obj.value(QStringLiteral("memberVisibility")).toString();
    d.version = readInt64(obj, QStringLiteral("version"));
    d.descendantMemberCount = readInt64(obj, QStringLiteral("descendantMemberCount"), -1);
    return d;
}

QJsonObject serializeOrgDepartment(const OrgDepartment &dept) {
    QJsonObject obj{
        {QStringLiteral("deptID"), dept.deptId},
        {QStringLiteral("parentID"), dept.parentId},
        {QStringLiteral("name"), dept.name},
        {QStringLiteral("ancestorPath"), dept.ancestorPath},
        {QStringLiteral("sortOrder"), dept.sortOrder},
        {QStringLiteral("version"), static_cast<double>(dept.version)},
    };
    if (!dept.deptCode.isEmpty()) {
        obj.insert(QStringLiteral("deptCode"), dept.deptCode);
    }
    if (!dept.memberVisibility.isEmpty()) {
        obj.insert(QStringLiteral("memberVisibility"), dept.memberVisibility);
    }
    if (dept.descendantMemberCount >= 0) {
        obj.insert(QStringLiteral("descendantMemberCount"), static_cast<double>(dept.descendantMemberCount));
    }
    return obj;
}

DeptMember parseDeptMember(const QJsonObject &obj) {
    DeptMember m;
    m.deptId = obj.value(QStringLiteral("deptID")).toString();
    m.isLeader = obj.value(QStringLiteral("isLeader")).toBool(false);
    m.sortOrder = obj.value(QStringLiteral("sortOrder")).toInt(0);
    if (obj.value(QStringLiteral("profile")).isObject()) {
        m.profile = parseUserProfile(obj.value(QStringLiteral("profile")).toObject());
    }
    return m;
}

ContactRelation parseContactRelation(const QJsonObject &obj) {
    ContactRelation rel;
    rel.ownerUserId = obj.value(QStringLiteral("ownerUserID")).toString();
    rel.targetUserId = obj.value(QStringLiteral("targetUserID")).toString();
    rel.remark = obj.value(QStringLiteral("remark")).toString();
    rel.isFavorite = obj.value(QStringLiteral("isFavorite")).toBool(false);
    rel.isBlocked = obj.value(QStringLiteral("isBlocked")).toBool(false);
    rel.groupIds = readStringList(obj.value(QStringLiteral("groupIDs")));
    const int src = obj.value(QStringLiteral("sourceType")).toInt(1);
    rel.sourceType = (src == 2) ? ContactSourceType::PersonalAdd : ContactSourceType::OrgColleague;
    rel.createdAtMs = readInt64(obj, QStringLiteral("createdAtMs"));
    rel.updatedAtMs = readInt64(obj, QStringLiteral("updatedAtMs"));
    if (obj.value(QStringLiteral("profile")).isObject()) {
        rel.profile = parseUserProfile(obj.value(QStringLiteral("profile")).toObject());
    }
    return rel;
}

QJsonObject serializeContactRelation(const ContactRelation &relation) {
    QJsonObject obj;
    obj.insert(QStringLiteral("ownerUserID"), relation.ownerUserId);
    obj.insert(QStringLiteral("targetUserID"), relation.targetUserId);
    obj.insert(QStringLiteral("remark"), relation.remark);
    obj.insert(QStringLiteral("isFavorite"), relation.isFavorite);
    obj.insert(QStringLiteral("isBlocked"), relation.isBlocked);
    obj.insert(QStringLiteral("groupIDs"), toJsonArray(relation.groupIds));
    obj.insert(QStringLiteral("sourceType"), static_cast<int>(relation.sourceType));
    obj.insert(QStringLiteral("createdAtMs"), static_cast<double>(relation.createdAtMs));
    obj.insert(QStringLiteral("updatedAtMs"), static_cast<double>(relation.updatedAtMs));
    if (relation.profile) {
        obj.insert(QStringLiteral("profile"), serializeUserProfile(*relation.profile));
    }
    return obj;
}

ContactGroup parseContactGroup(const QJsonObject &obj) {
    ContactGroup g;
    g.groupId = obj.value(QStringLiteral("groupID")).toString();
    g.name = obj.value(QStringLiteral("name")).toString();
    g.sortOrder = obj.value(QStringLiteral("sortOrder")).toInt(0);
    g.updatedAtMs = readInt64(obj, QStringLiteral("updatedAtMs"));
    return g;
}

QJsonObject serializeContactGroup(const ContactGroup &group) {
    return {
        {QStringLiteral("groupID"), group.groupId},
        {QStringLiteral("name"), group.name},
        {QStringLiteral("sortOrder"), group.sortOrder},
        {QStringLiteral("updatedAtMs"), static_cast<double>(group.updatedAtMs)},
    };
}

ContactRequest parseContactRequest(const QJsonObject &obj) {
    ContactRequest req;
    req.requestId = obj.value(QStringLiteral("requestID")).toString();
    req.fromUserId = obj.value(QStringLiteral("fromUserID")).toString();
    req.toUserId = obj.value(QStringLiteral("toUserID")).toString();
    req.greeting = obj.value(QStringLiteral("greeting")).toString();
    const int st = obj.value(QStringLiteral("status")).toInt(0);
    if (st == 1) req.status = ContactRequestStatus::Accepted;
    else if (st == 2) req.status = ContactRequestStatus::Rejected;
    else req.status = ContactRequestStatus::Pending;
    req.createdAtMs = readInt64(obj, QStringLiteral("createdAtMs"));
    req.handledAtMs = readInt64(obj, QStringLiteral("handledAtMs"));
    if (obj.value(QStringLiteral("fromProfile")).isObject()) {
        req.fromProfile = parseUserProfile(obj.value(QStringLiteral("fromProfile")).toObject());
    }
    if (obj.value(QStringLiteral("toProfile")).isObject()) {
        req.toProfile = parseUserProfile(obj.value(QStringLiteral("toProfile")).toObject());
    }
    return req;
}

UserProfileResponse parseUserProfileResponse(const QJsonObject &obj) {
    UserProfileResponse resp;
    if (obj.value(QStringLiteral("profile")).isObject()) {
        resp.profile = parseUserProfile(obj.value(QStringLiteral("profile")).toObject());
    }
    if (obj.value(QStringLiteral("relation")).isObject()) {
        resp.relation = parseContactRelation(obj.value(QStringLiteral("relation")).toObject());
    }
    if (obj.value(QStringLiteral("orgHistory")).isArray()) {
        for (const auto &v : obj.value(QStringLiteral("orgHistory")).toArray()) {
            if (!v.isObject()) continue;
            const auto o = v.toObject();
            OrgEmployeeRecord rec;
            rec.recordId = o.value(QStringLiteral("recordID")).toString();
            rec.orgCode = o.value(QStringLiteral("orgCode")).toString();
            rec.employeeNo = o.value(QStringLiteral("employeeNo")).toString();
            rec.jobTitle = o.value(QStringLiteral("jobTitle")).toString();
            rec.deptNames = readStringList(o.value(QStringLiteral("deptNames")));
            rec.status = o.value(QStringLiteral("status")).toInt(1);
            rec.joinedAtMs = readInt64(o, QStringLiteral("joinedAtMs"));
            rec.removedAtMs = readInt64(o, QStringLiteral("removedAtMs"));
            rec.removedReason = o.value(QStringLiteral("removedReason")).toString();
            if (!rec.employeeNo.isEmpty()) {
                resp.orgHistory.push_back(std::move(rec));
            }
        }
    }
    return resp;
}

DirectorySearchResponse parseDirectorySearchResponse(const QJsonObject &obj) {
    DirectorySearchResponse resp;
    resp.total = readInt64(obj, QStringLiteral("total"));
    for (const auto &v : obj.value(QStringLiteral("users")).toArray()) {
        if (v.isObject()) resp.users.push_back(parseUserProfile(v.toObject()));
    }
    return resp;
}

OrgTreeResponse parseOrgTreeResponse(const QJsonObject &obj) {
    OrgTreeResponse resp;
    resp.treeVersion = readInt64(obj, QStringLiteral("treeVersion"));
    resp.notModified = obj.value(QStringLiteral("notModified")).toBool(false);
    resp.memberVisibility = obj.value(QStringLiteral("memberVisibility")).toString();
    for (const auto &v : obj.value(QStringLiteral("departments")).toArray()) {
        if (v.isObject()) resp.departments.push_back(parseOrgDepartment(v.toObject()));
    }
    return resp;
}

DeptMembersResponse parseDeptMembersResponse(const QJsonObject &obj) {
    DeptMembersResponse resp;
    resp.deptId = obj.value(QStringLiteral("deptID")).toString();
    resp.total = readInt64(obj, QStringLiteral("total"));
    resp.restricted = obj.value(QStringLiteral("restricted")).toBool(false);
    for (const auto &v : obj.value(QStringLiteral("members")).toArray()) {
        if (v.isObject()) resp.members.push_back(parseDeptMember(v.toObject()));
    }
    return resp;
}

ContactSnapshotResponse parseContactSnapshotResponse(const QJsonObject &obj) {
    ContactSnapshotResponse resp;
    resp.serverTimeMs = readInt64(obj, QStringLiteral("serverTimeMs"));
    resp.pendingCount = readInt64(obj, QStringLiteral("pendingCount"));
    for (const auto &v : obj.value(QStringLiteral("contacts")).toArray()) {
        if (v.isObject()) resp.contacts.push_back(parseContactRelation(v.toObject()));
    }
    for (const auto &v : obj.value(QStringLiteral("groups")).toArray()) {
        if (v.isObject()) resp.groups.push_back(parseContactGroup(v.toObject()));
    }
    return resp;
}

ManageContactGroupResponse parseManageContactGroupResponse(const QJsonObject &obj) {
    ManageContactGroupResponse resp;
    if (obj.value(QStringLiteral("group")).isObject()) {
        resp.group = parseContactGroup(obj.value(QStringLiteral("group")).toObject());
    }
    for (const auto &v : obj.value(QStringLiteral("groups")).toArray()) {
        if (v.isObject()) resp.groups.push_back(parseContactGroup(v.toObject()));
    }
    return resp;
}

SendContactRequestResponse parseSendContactRequestResponse(const QJsonObject &obj) {
    SendContactRequestResponse resp;
    resp.autoAccepted = obj.value(QStringLiteral("autoAccepted")).toBool(false);
    if (obj.value(QStringLiteral("request")).isObject()) {
        resp.request = parseContactRequest(obj.value(QStringLiteral("request")).toObject());
    }
    if (obj.value(QStringLiteral("relation")).isObject()) {
        resp.relation = parseContactRelation(obj.value(QStringLiteral("relation")).toObject());
    }
    return resp;
}

HandleContactRequestResponse parseHandleContactRequestResponse(const QJsonObject &obj) {
    HandleContactRequestResponse resp;
    if (obj.value(QStringLiteral("request")).isObject()) {
        resp.request = parseContactRequest(obj.value(QStringLiteral("request")).toObject());
    }
    if (obj.value(QStringLiteral("relation")).isObject()) {
        resp.relation = parseContactRelation(obj.value(QStringLiteral("relation")).toObject());
    }
    return resp;
}

ContactRequestsResponse parseContactRequestsResponse(const QJsonObject &obj) {
    ContactRequestsResponse resp;
    resp.pendingCount = readInt64(obj, QStringLiteral("pendingCount"));
    for (const auto &v : obj.value(QStringLiteral("requests")).toArray()) {
        if (v.isObject()) resp.requests.push_back(parseContactRequest(v.toObject()));
    }
    return resp;
}

} // namespace OpenMeeting
