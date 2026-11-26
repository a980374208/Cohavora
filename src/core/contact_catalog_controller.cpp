#include "src/core/contact_catalog_controller.h"

#include "src/net/openmeeting_http_client.h"
#include "src/net/session_manager.h"
#include "src/telemetry/log_redaction.h"

#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace OpenMeeting {

ContactCatalogController::Backend ContactCatalogController::defaultBackend(SessionManager &session) {
    Backend b;
    b.syncContactSnapshot = [&session](qint64 updatedAfterMs, ResultCallback<ContactSnapshotResponse> cb) {
        session.httpClient().syncContactSnapshot(updatedAfterMs, std::move(cb));
    };
    b.getOrgTree = [&session](qint64 clientVersion, ResultCallback<OrgTreeResponse> cb) {
        session.httpClient().getOrgTree(clientVersion, std::move(cb));
    };
    b.getDeptMembers = [&session](const QString &deptId, qint64 offset, qint64 limit, ResultCallback<DeptMembersResponse> cb) {
        session.httpClient().getDeptMembers(deptId, offset, limit, std::move(cb));
    };
    b.searchDirectory = [&session](const QString &keyword, const QString &deptId, const QString &scope, qint64 offset, qint64 limit, ResultCallback<DirectorySearchResponse> cb) {
        session.httpClient().searchDirectory(keyword, deptId, scope, offset, limit, std::move(cb));
    };
    b.upsertRelation = [&session](const UpsertContactRelationRequest &req, ResultCallback<ContactRelation> cb) {
        session.httpClient().upsertContactRelation(req, std::move(cb));
    };
    b.removeRelation = [&session](const QString &targetId, ResultCallback<bool> cb) {
        session.httpClient().removeContactRelation(targetId, std::move(cb));
    };
    b.manageGroup = [&session](const ManageContactGroupRequest &req, ResultCallback<ManageContactGroupResponse> cb) {
        session.httpClient().manageContactGroup(req, std::move(cb));
    };
    b.sendRequest = [&session](const QString &toUserId, const QString &greeting, ResultCallback<SendContactRequestResponse> cb) {
        session.httpClient().sendContactRequest(toUserId, greeting, std::move(cb));
    };
    b.handleRequest = [&session](const QString &requestId, bool accept, ResultCallback<HandleContactRequestResponse> cb) {
        session.httpClient().handleContactRequest(requestId, accept, std::move(cb));
    };
    b.listRequests = [&session](qint64 limit, ResultCallback<ContactRequestsResponse> cb) {
        session.httpClient().listContactRequests(limit, std::move(cb));
    };
    b.getUserProfile = [&session](const QString &targetId, ResultCallback<UserProfileResponse> cb) {
        session.httpClient().getUserProfile(targetId, std::move(cb));
    };
    b.updateUserProfile = [&session](
            const std::optional<QString> &nickname,
            const std::optional<QString> &signature,
            const std::optional<QString> &email,
            const std::optional<QString> &phone,
            ResultCallback<UserProfile> cb) {
        session.httpClient().updateUserProfile(nickname, signature, email, phone, std::move(cb));
    };
    return b;
}

ContactCatalogController::ContactCatalogController(SessionManager &session, QObject *parent)
    : ContactCatalogController(session, defaultBackend(session), ContactSnapshotStore{}, parent) {
}

ContactCatalogController::ContactCatalogController(
        SessionManager &session, Backend backend, ContactSnapshotStore store, QObject *parent)
    : QObject(parent)
    , _session(session)
    , _backend(std::move(backend))
    , _store(std::move(store)) {
    connect(&_session, &SessionManager::authenticationReset, this,
            [this](quint64) { handleAccountChange(false); });
    connect(&_session, &SessionManager::loggedIn, this,
            [this](const UserInfo &) { handleAccountChange(true); });
    connect(&_session, &SessionManager::loggedOut, this,
            [this] { handleAccountChange(false); });
    connect(&_session, &SessionManager::sessionInvalidated, this,
            [this](SessionInvalidationReason) { handleAccountChange(false); });

    if (_session.isLoggedIn()) {
        handleAccountChange(true);
    }
}

std::optional<ContactCatalogController::RequestContext>
ContactCatalogController::captureContext() const {
    if (!_session.isLoggedIn()) return std::nullopt;
    RequestContext ctx;
    ctx.authGeneration = _session.authGeneration();
    ctx.httpRevision = _session.httpClient().authRevision();
    ctx.sessionService = _session.serverBaseUrl();
    ctx.httpService = _session.httpClient().baseUrl();
    ctx.userId = _session.userId();
    if (ctx.userId.isEmpty()) return std::nullopt;
    return ctx;
}

bool ContactCatalogController::isContextCurrent(const RequestContext &context) const {
    return _session.isLoggedIn() &&
        _session.authGeneration() == context.authGeneration &&
        _session.httpClient().authRevision() == context.httpRevision &&
        _session.serverBaseUrl() == context.sessionService &&
        _session.httpClient().baseUrl() == context.httpService &&
        _session.userId() == context.userId;
}

void ContactCatalogController::handleAccountChange(bool loggedIn) {
    ++_snapshotGeneration;
    ++_orgTreeGeneration;
    ++_searchGeneration;
    ++_requestsGeneration;

    _contacts.clear();
    _groups.clear();
    _departments.clear();
    _deptMembers.clear();
    _restrictedDeptIds.clear();
    _deptMemberRequests.clear();
    _nextDeptMemberOffsets.clear();
    _orgMemberVisibility = QStringLiteral("all");
    _requests.clear();
    _searchResults.clear();
    _profileIndex.clear();
    _selfProfile.reset();
    _selfOrgHistory.clear();
    _syncSeq = 0;
    _treeVersion = 0;
    _serverTimeMs = 0;
    _pendingRequestCount = 0;
    _loading = false;
    _lastError.clear();

    if (loggedIn && _session.isLoggedIn()) {
        const QString key = ContactSnapshotStore::accountScopeKey(true, _session.userId(), _session.serverBaseUrl());
        const auto cached = _store.load(key);
        _syncSeq = cached.syncSeq;
        _treeVersion = cached.treeVersion;
        _serverTimeMs = cached.serverTimeMs;
        _contacts = cached.contacts;
        _groups = cached.groups;
        _departments = cached.departments;
        cacheContactProfiles();
    }

    emit contactsChanged();
    emit groupsChanged();
    emit orgTreeChanged();
    emit requestsChanged();
    emit pendingRequestCountChanged(_pendingRequestCount);
    emit searchResultsChanged();
    emit selfProfileChanged();
    emit loadingStateChanged(false);

    if (loggedIn && _session.isLoggedIn()) {
        refreshAll();
    }
}

void ContactCatalogController::persistSnapshot() {
    if (!_session.isLoggedIn()) return;
    ContactCacheSnapshot snap;
    snap.syncSeq = _syncSeq;
    snap.treeVersion = _treeVersion;
    snap.serverTimeMs = _serverTimeMs;
    snap.contacts = _contacts;
    snap.groups = _groups;
    snap.departments = _departments;
    const QString key = ContactSnapshotStore::accountScopeKey(true, _session.userId(), _session.serverBaseUrl());
    _store.save(key, snap);
}

void ContactCatalogController::cacheProfile(const UserProfile &profile) {
    if (profile.userId.isEmpty()) return;
    _profileIndex.insert(profile.userId, profile);
    for (auto &rel : _contacts) {
        if (rel.targetUserId == profile.userId) {
            rel.profile = profile;
        }
    }
    for (auto it = _deptMembers.begin(); it != _deptMembers.end(); ++it) {
        for (auto &m : it.value()) {
            if (m.profile.userId == profile.userId) {
                m.profile = profile;
            }
        }
    }
}

void ContactCatalogController::cacheContactProfiles() {
    QHash<QString, UserProfile> snapshotProfiles;
    snapshotProfiles.reserve(static_cast<qsizetype>(_contacts.size()));
    for (const auto &rel : _contacts) {
        if (rel.profile && !rel.profile->userId.isEmpty() &&
            !snapshotProfiles.contains(rel.profile->userId)) {
            snapshotProfiles.insert(rel.profile->userId, *rel.profile);
        }
    }
    if (snapshotProfiles.isEmpty()) return;

    // Update matching rows from the contact list. The shared index can also
    // contain profiles from earlier requests and other endpoints.
    for (auto it = snapshotProfiles.cbegin(); it != snapshotProfiles.cend(); ++it) {
        _profileIndex.insert(it.key(), it.value());
    }
    for (auto &rel : _contacts) {
        const auto profile = snapshotProfiles.constFind(rel.targetUserId);
        if (profile != snapshotProfiles.cend()) rel.profile = profile.value();
    }
    for (auto &members : _deptMembers) {
        for (auto &member : members) {
            const auto profile = snapshotProfiles.constFind(member.profile.userId);
            if (profile != snapshotProfiles.cend()) member.profile = profile.value();
        }
    }
}

std::vector<DeptMember> ContactCatalogController::deptMembers(const QString &deptId) const {
    return _deptMembers.value(deptId);
}

std::optional<UserProfile> ContactCatalogController::findProfile(const QString &userId) const {
    const auto it = _profileIndex.constFind(userId);
    if (it != _profileIndex.constEnd()) return *it;
    return std::nullopt;
}

std::optional<ContactRelation> ContactCatalogController::findRelation(const QString &targetUserId) const {
    for (const auto &rel : _contacts) {
        if (rel.targetUserId == targetUserId) return rel;
    }
    return std::nullopt;
}

QString ContactCatalogController::displayNameForUser(const QString &userId) const {
    if (const auto rel = findRelation(userId)) {
        return rel->effectiveDisplayName();
    }
    if (const auto prof = findProfile(userId)) {
        return prof->displayName();
    }
    return userId;
}

QStringList ContactCatalogController::trackedContactUserIds(int maxCount) const {
    QStringList ids;
    ids.reserve(std::min(static_cast<int>(_contacts.size()), maxCount));
    for (const auto &rel : _contacts) {
        if (rel.isBlocked || rel.targetUserId.isEmpty()) continue;
        ids.push_back(rel.targetUserId);
        if (ids.size() >= maxCount) break;
    }
    return ids;
}

bool ContactCatalogController::refreshAll() {
    const bool a = refreshContacts(true);
    const bool b = refreshOrgTree(true);
    const bool c = refreshRequests();
    const bool d = fetchUserProfile(_session.userId());
    return a || b || c || d;
}

bool ContactCatalogController::refreshContacts(bool forceFull) {
    const auto ctx = captureContext();
    if (!ctx || !_backend.syncContactSnapshot) return false;

    const quint64 gen = ++_snapshotGeneration;
    _loading = true;
    emit loadingStateChanged(true);
    const qint64 sinceMs = forceFull ? 0 : _serverTimeMs;
    const QPointer<ContactCatalogController> self(this);

    _backend.syncContactSnapshot(sinceMs, [self, ctx = *ctx, gen, forceFull](
            bool ok, const ContactSnapshotResponse &resp, const HttpError &err) {
        if (!self || gen != self->_snapshotGeneration || !self->isContextCurrent(ctx)) return;
        self->_loading = false;
        emit self->loadingStateChanged(false);
        if (!ok) {
            self->_lastError = err.message;
            spdlog::warn("[Contacts] sync_snapshot failed {}",
                         livekit::secure_log::ErrorCodeSummary("sync_snapshot", err.code, "http"));
            emit self->errorOccurred(err.message);
            return;
        }
        self->_lastError.clear();
        self->_serverTimeMs = resp.serverTimeMs;
        if (forceFull) {
            self->_contacts = resp.contacts;
        } else {
            for (const auto &rel : resp.contacts) {
                self->mergeContactRelation(rel);
            }
        }
        self->cacheContactProfiles();
        self->_groups = resp.groups;
        self->_pendingRequestCount = resp.pendingCount;
        self->persistSnapshot();
        emit self->contactsChanged();
        emit self->groupsChanged();
        emit self->pendingRequestCountChanged(self->_pendingRequestCount);
    });
    return true;
}

bool ContactCatalogController::refreshOrgTree(bool forceFull) {
    const auto ctx = captureContext();
    if (!ctx || !_backend.getOrgTree) return false;
    const quint64 gen = ++_orgTreeGeneration;
    const qint64 reqVersion = forceFull ? 0 : _treeVersion;
    const QPointer<ContactCatalogController> self(this);
    if (forceFull) {
        // Revocation can arrive while a member request is still in flight.
        _deptMemberRequests.clear();
        auto changedDeptIds = _restrictedDeptIds;
        for (auto it = _deptMembers.cbegin(); it != _deptMembers.cend(); ++it) {
            changedDeptIds.insert(it.key());
        }
        _deptMembers.clear();
        _restrictedDeptIds.clear();
        _nextDeptMemberOffsets.clear();
        for (const auto &deptId : changedDeptIds) {
            emit deptMembersChanged(deptId);
            if (!self || self->_orgTreeGeneration != gen || !self->isContextCurrent(*ctx)) return false;
        }
    }
    _backend.getOrgTree(reqVersion, [self, ctx = *ctx, gen, forceFull](
            bool ok, const OrgTreeResponse &resp, const HttpError &err) {
        if (!self || gen != self->_orgTreeGeneration || !self->isContextCurrent(ctx)) return;
        if (!ok) {
            spdlog::warn("[Contacts] get_org_tree failed {}",
                         livekit::secure_log::ErrorCodeSummary("get_org_tree", err.code, "http"));
            if (forceFull) emit self->orgTreeChanged();
            return;
        }
        if (resp.notModified && !forceFull) return;
        self->_treeVersion = resp.treeVersion;
        if (!resp.memberVisibility.isEmpty()) {
            self->_orgMemberVisibility = resp.memberVisibility;
        } else if (resp.departments.empty()) {
            self->_orgMemberVisibility = QStringLiteral("all");
        }
        self->_departments = resp.departments;
        // Even surviving department IDs can have different membership permissions.
        self->_deptMemberRequests.clear();
        self->_nextDeptMemberOffsets.clear();
        self->_deptMembers.clear();
        self->_restrictedDeptIds.clear();
        self->persistSnapshot();
        emit self->orgTreeChanged();
    });
    return true;
}

bool ContactCatalogController::ensureDeptMembersLoaded(const QString &deptId) {
    return !hasDeptMembersResult(deptId) && !isDeptMembersLoading(deptId) && loadDeptMembers(deptId);
}

bool ContactCatalogController::loadMoreDeptMembers(const QString &deptId) {
    const auto offset = _nextDeptMemberOffsets.constFind(deptId);
    return offset != _nextDeptMemberOffsets.cend() && !isDeptMembersLoading(deptId) &&
        loadDeptMembers(deptId, offset.value());
}

bool ContactCatalogController::loadDeptMembers(const QString &deptId, qint64 offset, qint64 limit) {
    const auto ctx = captureContext();
    if (!ctx || deptId.trimmed().isEmpty() || !_backend.getDeptMembers) return false;
    offset = std::max<qint64>(offset, 0);
    if (limit <= 0 || limit > 200) limit = 100;
    const quint64 requestId = ++_nextDeptMemberRequest;
    _deptMemberRequests.insert(deptId, requestId);
    const QPointer<ContactCatalogController> self(this);
    _backend.getDeptMembers(deptId, offset, limit, [self, ctx = *ctx, deptId, offset, limit, requestId](
            bool ok, const DeptMembersResponse &resp, const HttpError &err) {
        if (!self || !self->isContextCurrent(ctx) ||
            self->_deptMemberRequests.value(deptId) != requestId) return;
        self->_deptMemberRequests.remove(deptId);
        if (!ok) {
            spdlog::warn("[Contacts] get_dept_members failed {}",
                         livekit::secure_log::ErrorCodeSummary("get_dept_members", err.code, "http"));
            emit self->errorOccurred(err.message);
            if (self && offset > 0 && self->isContextCurrent(ctx)) emit self->deptMembersChanged(deptId);
            return;
        }
        if (resp.restricted) {
            self->_deptMembers.remove(deptId);
            self->_nextDeptMemberOffsets.remove(deptId);
            self->_restrictedDeptIds.insert(deptId);
            emit self->deptMembersChanged(deptId);
            return;
        }
        for (const auto &m : resp.members) {
            self->cacheProfile(m.profile);
        }
        self->_restrictedDeptIds.remove(deptId);
        // The server pages membership records before omitting disabled or missing users.
        if (resp.total > offset && resp.total - offset > limit) {
            self->_nextDeptMemberOffsets.insert(deptId, offset + limit);
        } else {
            self->_nextDeptMemberOffsets.remove(deptId);
        }
        if (offset == 0) {
            self->_deptMembers.insert(deptId, resp.members);
        } else {
            auto &list = self->_deptMembers[deptId];
            QSet<QString> knownUsers;
            for (const auto &member : list) knownUsers.insert(member.profile.userId);
            for (const auto &member : resp.members) {
                if (knownUsers.contains(member.profile.userId)) continue;
                knownUsers.insert(member.profile.userId);
                list.push_back(member);
            }
        }
        emit self->deptMembersChanged(deptId);
    });
    if (self && offset > 0 && self->_deptMemberRequests.value(deptId) == requestId) {
        emit self->deptMembersChanged(deptId);
    }
    return true;
}

bool ContactCatalogController::refreshRequests() {
    const auto ctx = captureContext();
    if (!ctx || !_backend.listRequests) return false;
    const quint64 gen = ++_requestsGeneration;
    const QPointer<ContactCatalogController> self(this);
    _backend.listRequests(100, [self, ctx = *ctx, gen](
            bool ok, const ContactRequestsResponse &resp, const HttpError &err) {
        if (!self || gen != self->_requestsGeneration || !self->isContextCurrent(ctx)) return;
        if (!ok) {
            spdlog::warn("[Contacts] list_requests failed {}",
                         livekit::secure_log::ErrorCodeSummary("list_requests", err.code, "http"));
            return;
        }
        self->_requests = resp.requests;
        for (const auto &r : self->_requests) {
            if (r.fromProfile) self->cacheProfile(*r.fromProfile);
            if (r.toProfile) self->cacheProfile(*r.toProfile);
        }
        self->_pendingRequestCount = resp.pendingCount;
        emit self->requestsChanged();
        emit self->pendingRequestCountChanged(self->_pendingRequestCount);
    });
    return true;
}

bool ContactCatalogController::searchDirectory(const QString &keyword, const QString &deptId, const QString &scope) {
    const auto ctx = captureContext();
    if (!ctx || !_backend.searchDirectory) return false;
    const quint64 gen = ++_searchGeneration;
    const QPointer<ContactCatalogController> self(this);
    _backend.searchDirectory(keyword, deptId, scope, 0, 50, [self, ctx = *ctx, gen](
            bool ok, const DirectorySearchResponse &resp, const HttpError &err) {
        if (!self || gen != self->_searchGeneration || !self->isContextCurrent(ctx)) return;
        if (!ok) {
            spdlog::warn("[Contacts] directory_search failed {}",
                         livekit::secure_log::ErrorCodeSummary("directory_search", err.code, "http"));
            emit self->errorOccurred(err.message);
            return;
        }
        self->_searchResults = resp.users;
        for (const auto &u : self->_searchResults) {
            self->cacheProfile(u);
        }
        emit self->searchResultsChanged();
    });
    return true;
}

bool ContactCatalogController::lookupContactRequestRecipient(
        const QString &identifier, ResultCallback<std::optional<UserProfile>> callback) {
    const auto ctx = captureContext();
    const QString keyword = identifier.trimmed();
    if (!ctx || !_backend.searchDirectory || keyword.isEmpty() || !callback) return false;
    const QPointer<ContactCatalogController> self(this);
    _backend.searchDirectory(keyword, {}, QStringLiteral("exact"), 0, 1,
        [self, ctx = *ctx, cb = std::move(callback)](
                bool ok, const DirectorySearchResponse &resp, const HttpError &err) {
            if (!self || !self->isContextCurrent(ctx)) return;
            std::optional<UserProfile> recipient;
            if (ok && resp.users.size() == 1) {
                const auto &match = resp.users.front();
                if (!match.userId.trimmed().isEmpty() && match.userId != ctx.userId) {
                    recipient = match;
                }
            }
            cb(ok, recipient, err);
        });
    return true;
}

void ContactCatalogController::clearSearchResults() {
    ++_searchGeneration;
    if (_searchResults.empty()) return;
    _searchResults.clear();
    emit searchResultsChanged();
}

bool ContactCatalogController::upsertRelation(
        const UpsertContactRelationRequest &request, ResultCallback<ContactRelation> callback) {
    const auto ctx = captureContext();
    if (!ctx || !_backend.upsertRelation) return false;
    const QPointer<ContactCatalogController> self(this);
    _backend.upsertRelation(request, [self, ctx = *ctx, targetId = request.targetUserId, cb = std::move(callback)](
            bool ok, const ContactRelation &rel, const HttpError &err) {
        if (!self || !self->isContextCurrent(ctx)) return;
        if (ok) {
            if (rel.targetUserId.isEmpty()) {
                self->removeContactRelationLocal(targetId);
            } else {
                self->mergeContactRelation(rel);
            }
            self->persistSnapshot();
            emit self->contactsChanged();
        }
        if (cb) cb(ok, rel, err);
    });
    return true;
}

bool ContactCatalogController::removeRelation(const QString &targetUserId, ResultCallback<bool> callback) {
    const auto ctx = captureContext();
    if (!ctx || !_backend.removeRelation) return false;
    const QPointer<ContactCatalogController> self(this);
    _backend.removeRelation(targetUserId, [self, ctx = *ctx, targetUserId, cb = std::move(callback)](
            bool ok, const bool &removed, const HttpError &err) {
        if (!self || !self->isContextCurrent(ctx)) return;
        if (ok && removed) {
            self->removeContactRelationLocal(targetUserId);
            self->persistSnapshot();
            emit self->contactsChanged();
        }
        if (cb) cb(ok, removed, err);
    });
    return true;
}

bool ContactCatalogController::manageGroup(
        const ManageContactGroupRequest &request, ResultCallback<ManageContactGroupResponse> callback) {
    const auto ctx = captureContext();
    if (!ctx || !_backend.manageGroup) return false;
    const QPointer<ContactCatalogController> self(this);
    _backend.manageGroup(request, [self, ctx = *ctx, cb = std::move(callback)](
            bool ok, const ManageContactGroupResponse &resp, const HttpError &err) {
        if (!self || !self->isContextCurrent(ctx)) return;
        if (ok) {
            self->_groups = resp.groups;
            self->persistSnapshot();
            emit self->groupsChanged();
        }
        if (cb) cb(ok, resp, err);
    });
    return true;
}

bool ContactCatalogController::sendContactRequest(
        const QString &toUserId, const QString &greeting, ResultCallback<SendContactRequestResponse> callback) {
    const auto ctx = captureContext();
    if (!ctx || !_backend.sendRequest) return false;
    const QPointer<ContactCatalogController> self(this);
    _backend.sendRequest(toUserId, greeting, [self, ctx = *ctx, cb = std::move(callback)](
            bool ok, const SendContactRequestResponse &resp, const HttpError &err) {
        if (!self || !self->isContextCurrent(ctx)) return;
        if (ok) {
            if (resp.relation) {
                self->mergeContactRelation(*resp.relation);
                self->persistSnapshot();
                emit self->contactsChanged();
            }
            if (resp.request) {
                self->refreshRequests();
            }
        }
        if (cb) cb(ok, resp, err);
    });
    return true;
}

bool ContactCatalogController::handleContactRequest(
        const QString &requestId, bool accept, ResultCallback<HandleContactRequestResponse> callback) {
    const auto ctx = captureContext();
    if (!ctx || !_backend.handleRequest) return false;
    const QPointer<ContactCatalogController> self(this);
    _backend.handleRequest(requestId, accept, [self, ctx = *ctx, cb = std::move(callback)](
            bool ok, const HandleContactRequestResponse &resp, const HttpError &err) {
        if (!self || !self->isContextCurrent(ctx)) return;
        if (ok) {
            for (auto &r : self->_requests) {
                if (r.requestId == resp.request.requestId) {
                    r = resp.request;
                    break;
                }
            }
            if (resp.relation) {
                self->mergeContactRelation(*resp.relation);
                self->persistSnapshot();
                emit self->contactsChanged();
            }
            self->recomputePendingCount();
            emit self->requestsChanged();
        }
        if (cb) cb(ok, resp, err);
    });
    return true;
}

void ContactCatalogController::advanceSyncSeq(qint64 seq) {
    if (seq > _syncSeq) {
        _syncSeq = seq;
        persistSnapshot();
    }
}

void ContactCatalogController::applyRelationEvent(const QJsonObject &payload) {
    if (payload.value(QStringLiteral("removed")).toBool(false)) {
        removeContactRelationLocal(payload.value(QStringLiteral("targetUserID")).toString());
    } else {
        mergeContactRelation(parseContactRelation(payload));
    }
    persistSnapshot();
    emit contactsChanged();
}

void ContactCatalogController::applyRequestReceivedEvent(const QJsonObject &payload) {
    const auto req = parseContactRequest(payload);
    if (req.requestId.isEmpty()) return;
    if (req.fromProfile) cacheProfile(*req.fromProfile);
    if (req.toProfile) cacheProfile(*req.toProfile);
    auto it = std::find_if(_requests.begin(), _requests.end(),
        [&](const ContactRequest &r) { return r.requestId == req.requestId; });
    if (it != _requests.end()) *it = req;
    else _requests.insert(_requests.begin(), req);
    recomputePendingCount();
    emit requestsChanged();
}

void ContactCatalogController::applyRequestHandledEvent(const QJsonObject &payload) {
    const auto req = parseContactRequest(payload);
    if (req.requestId.isEmpty()) return;
    auto it = std::find_if(_requests.begin(), _requests.end(),
        [&](const ContactRequest &r) { return r.requestId == req.requestId; });
    if (it != _requests.end()) *it = req;
    else _requests.insert(_requests.begin(), req);
    recomputePendingCount();
    emit requestsChanged();
}

bool ContactCatalogController::fetchUserProfile(
        const QString &targetUserId, ResultCallback<UserProfileResponse> callback) {
    const auto ctx = captureContext();
    if (!ctx || !_backend.getUserProfile) return false;
    const QString effectiveTarget = targetUserId.trimmed().isEmpty() ? ctx->userId : targetUserId.trimmed();
    const bool isSelf = (effectiveTarget == ctx->userId);
    const QPointer<ContactCatalogController> self(this);
    _backend.getUserProfile(effectiveTarget, [self, ctx = *ctx, isSelf, cb = std::move(callback)](
            bool ok, const UserProfileResponse &resp, const HttpError &err) {
        if (!self || !self->isContextCurrent(ctx)) return;
        if (ok && !resp.profile.userId.isEmpty()) {
            self->cacheProfile(resp.profile);
            if (isSelf) {
                self->_selfProfile = resp.profile;
                self->_selfOrgHistory = resp.orgHistory;
                if (!resp.profile.nickname.trimmed().isEmpty()) {
                    self->_session.updateCurrentUserNickname(resp.profile.nickname);
                }
                emit self->selfProfileChanged();
            }
            emit self->userProfileUpdated(resp.profile.userId);
        }
        if (cb) cb(ok, resp, err);
    });
    return true;
}

bool ContactCatalogController::updateSelfProfile(
        const std::optional<QString> &nickname,
        const std::optional<QString> &signature,
        const std::optional<QString> &email,
        const std::optional<QString> &phone,
        ResultCallback<UserProfile> callback) {
    const auto ctx = captureContext();
    if (!ctx || !_backend.updateUserProfile) return false;
    const QPointer<ContactCatalogController> self(this);
    _backend.updateUserProfile(nickname, signature, email, phone,
        [self, ctx = *ctx, cb = std::move(callback)](
                bool ok, const UserProfile &profile, const HttpError &err) {
            if (!self || !self->isContextCurrent(ctx)) return;
            if (ok && !profile.userId.isEmpty()) {
                self->cacheProfile(profile);
                self->_selfProfile = profile;
                if (!profile.nickname.trimmed().isEmpty()) {
                    self->_session.updateCurrentUserNickname(profile.nickname);
                }
                self->persistSnapshot();
                emit self->selfProfileChanged();
                emit self->userProfileUpdated(profile.userId);
                emit self->contactsChanged();
            }
            if (cb) cb(ok, profile, err);
        });
    return true;
}

void ContactCatalogController::applyOrgTreeUpdatedEvent(qint64 version) {
    if (version == 0 || version != _treeVersion) {
        refreshOrgTree(true);
        refreshContacts(true);
        fetchUserProfile(_session.userId());
    }
}

void ContactCatalogController::handleSyncResetRequired() {
    refreshAll();
}

void ContactCatalogController::mergeContactRelation(const ContactRelation &rel) {
    if (rel.targetUserId.isEmpty()) return;
    if (rel.profile) cacheProfile(*rel.profile);
    auto it = std::find_if(_contacts.begin(), _contacts.end(),
        [&](const ContactRelation &c) { return c.targetUserId == rel.targetUserId; });
    if (it != _contacts.end()) {
        auto merged = rel;
        if (!merged.profile && it->profile) merged.profile = it->profile;
        *it = std::move(merged);
    } else {
        _contacts.push_back(rel);
    }
}

void ContactCatalogController::removeContactRelationLocal(const QString &targetUserId) {
    _contacts.erase(std::remove_if(_contacts.begin(), _contacts.end(),
        [&](const ContactRelation &c) { return c.targetUserId == targetUserId; }),
        _contacts.end());
}

void ContactCatalogController::recomputePendingCount() {
    qint64 count = 0;
    const QString me = _session.userId();
    for (const auto &r : _requests) {
        if (r.toUserId == me && r.status == ContactRequestStatus::Pending) {
            ++count;
        }
    }
    if (count != _pendingRequestCount) {
        _pendingRequestCount = count;
        emit pendingRequestCountChanged(_pendingRequestCount);
    }
}

} // namespace OpenMeeting
