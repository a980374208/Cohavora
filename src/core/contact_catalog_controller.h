#pragma once

#include "src/core/contact_snapshot_store.h"
#include "src/net/contact_types.h"
#include "src/net/http_types.h"

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QSet>

#include <functional>
#include <optional>
#include <vector>

namespace OpenMeeting {

class SessionManager;

class ContactCatalogController final : public QObject {
    Q_OBJECT
public:
    struct Backend {
        std::function<void(qint64, ResultCallback<ContactSnapshotResponse>)> syncContactSnapshot;
        std::function<void(qint64, ResultCallback<OrgTreeResponse>)> getOrgTree;
        std::function<void(const QString &, qint64, qint64, ResultCallback<DeptMembersResponse>)> getDeptMembers;
        std::function<void(const QString &, const QString &, const QString &, qint64, qint64, ResultCallback<DirectorySearchResponse>)> searchDirectory;
        std::function<void(const UpsertContactRelationRequest &, ResultCallback<ContactRelation>)> upsertRelation;
        std::function<void(const QString &, ResultCallback<bool>)> removeRelation;
        std::function<void(const ManageContactGroupRequest &, ResultCallback<ManageContactGroupResponse>)> manageGroup;
        std::function<void(const QString &, const QString &, ResultCallback<SendContactRequestResponse>)> sendRequest;
        std::function<void(const QString &, bool, ResultCallback<HandleContactRequestResponse>)> handleRequest;
        std::function<void(qint64, ResultCallback<ContactRequestsResponse>)> listRequests;
        std::function<void(const QString &, ResultCallback<UserProfileResponse>)> getUserProfile;
        std::function<void(const std::optional<QString> &, const std::optional<QString> &, const std::optional<QString> &, const std::optional<QString> &, ResultCallback<UserProfile>)> updateUserProfile;
    };

    explicit ContactCatalogController(SessionManager &session, QObject *parent = nullptr);
    ContactCatalogController(SessionManager &session, Backend backend, ContactSnapshotStore store, QObject *parent = nullptr);

    // Authoritative read-only projections for UI / Copilot
    const std::vector<ContactRelation> &contacts() const { return _contacts; }
    const std::vector<ContactGroup> &groups() const { return _groups; }
    const std::vector<OrgDepartment> &departments() const { return _departments; }
    const std::vector<ContactRequest> &requests() const { return _requests; }
    const std::vector<UserProfile> &searchResults() const { return _searchResults; }
    const std::optional<UserProfile> &selfProfile() const { return _selfProfile; }
    const std::vector<OrgEmployeeRecord> &selfOrgHistory() const { return _selfOrgHistory; }
    std::vector<DeptMember> deptMembers(const QString &deptId) const;
    qint64 pendingRequestCount() const { return _pendingRequestCount; }
    qint64 syncSeq() const { return _syncSeq; }
    qint64 treeVersion() const { return _treeVersion; }
    bool hasOrganization() const { return !_departments.empty(); }
    const QString &orgMemberVisibility() const { return _orgMemberVisibility; }
    bool isDeptMembersRestricted(const QString &deptId) const { return _restrictedDeptIds.contains(deptId); }
    bool hasDeptMembersResult(const QString &deptId) const {
        return _deptMembers.contains(deptId) || isDeptMembersRestricted(deptId);
    }
    bool hasMoreDeptMembers(const QString &deptId) const { return _nextDeptMemberOffsets.contains(deptId); }
    bool isDeptMembersLoading(const QString &deptId) const { return _deptMemberRequests.contains(deptId); }
    bool isLoading() const { return _loading; }
    const QString &lastError() const { return _lastError; }

    std::optional<UserProfile> findProfile(const QString &userId) const;
    std::optional<ContactRelation> findRelation(const QString &targetUserId) const;
    QString displayNameForUser(const QString &userId) const;
    QStringList trackedContactUserIds(int maxCount = 500) const;

    // Actions
    bool refreshAll();
    bool refreshContacts(bool forceFull = false);
    bool refreshOrgTree(bool forceFull = false);
    bool loadDeptMembers(const QString &deptId, qint64 offset = 0, qint64 limit = 100);
    bool ensureDeptMembersLoaded(const QString &deptId);
    bool loadMoreDeptMembers(const QString &deptId);
    bool refreshRequests();
    bool searchDirectory(const QString &keyword, const QString &deptId = {}, const QString &scope = QStringLiteral("all"));
    bool lookupContactRequestRecipient(
        const QString &identifier, ResultCallback<std::optional<UserProfile>> callback);
    void clearSearchResults();

    bool upsertRelation(const UpsertContactRelationRequest &request, ResultCallback<ContactRelation> callback = {});
    bool removeRelation(const QString &targetUserId, ResultCallback<bool> callback = {});
    bool manageGroup(const ManageContactGroupRequest &request, ResultCallback<ManageContactGroupResponse> callback = {});
    bool sendContactRequest(const QString &toUserId, const QString &greeting, ResultCallback<SendContactRequestResponse> callback = {});
    bool handleContactRequest(const QString &requestId, bool accept, ResultCallback<HandleContactRequestResponse> callback = {});
    bool fetchUserProfile(const QString &targetUserId = {}, ResultCallback<UserProfileResponse> callback = {});
    bool updateSelfProfile(
        const std::optional<QString> &nickname,
        const std::optional<QString> &signature,
        const std::optional<QString> &email,
        const std::optional<QString> &phone,
        ResultCallback<UserProfile> callback = {});

    // Incremental event stream hooks
    void advanceSyncSeq(qint64 seq);
    void applyRelationEvent(const QJsonObject &payload);
    void applyRequestReceivedEvent(const QJsonObject &payload);
    void applyRequestHandledEvent(const QJsonObject &payload);
    void applyOrgTreeUpdatedEvent(qint64 version);
    void handleSyncResetRequired();

signals:
    void contactsChanged();
    void groupsChanged();
    void orgTreeChanged();
    void deptMembersChanged(const QString &deptId);
    void requestsChanged();
    void pendingRequestCountChanged(qint64 count);
    void searchResultsChanged();
    void selfProfileChanged();
    void userProfileUpdated(const QString &userId);
    void loadingStateChanged(bool loading);
    void errorOccurred(const QString &message);

private:
    struct RequestContext {
        quint64 authGeneration = 0;
        quint64 httpRevision = 0;
        QString sessionService;
        QString httpService;
        QString userId;
    };

    static Backend defaultBackend(SessionManager &session);
    std::optional<RequestContext> captureContext() const;
    bool isContextCurrent(const RequestContext &context) const;
    void handleAccountChange(bool loggedIn);
    void persistSnapshot();
    void cacheProfile(const UserProfile &profile);
    void cacheContactProfiles();
    void mergeContactRelation(const ContactRelation &rel);
    void removeContactRelationLocal(const QString &targetUserId);
    void recomputePendingCount();

    SessionManager &_session;
    Backend _backend;
    ContactSnapshotStore _store;

    std::vector<ContactRelation> _contacts;
    std::vector<ContactGroup> _groups;
    std::vector<OrgDepartment> _departments;
    QHash<QString, std::vector<DeptMember>> _deptMembers;
    QSet<QString> _restrictedDeptIds;
    QHash<QString, quint64> _deptMemberRequests;
    QHash<QString, qint64> _nextDeptMemberOffsets;
    QString _orgMemberVisibility = QStringLiteral("all");
    std::vector<ContactRequest> _requests;
    std::vector<UserProfile> _searchResults;
    QHash<QString, UserProfile> _profileIndex;
    std::optional<UserProfile> _selfProfile;
    std::vector<OrgEmployeeRecord> _selfOrgHistory;

    qint64 _syncSeq = 0;
    qint64 _treeVersion = 0;
    qint64 _serverTimeMs = 0;
    qint64 _pendingRequestCount = 0;
    bool _loading = false;
    QString _lastError;

    quint64 _snapshotGeneration = 0;
    quint64 _orgTreeGeneration = 0;
    quint64 _nextDeptMemberRequest = 0;
    quint64 _searchGeneration = 0;
    quint64 _requestsGeneration = 0;
};

} // namespace OpenMeeting
