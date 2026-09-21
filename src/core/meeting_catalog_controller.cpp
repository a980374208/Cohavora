#include "src/core/meeting_catalog_controller.h"

#include "src/net/openmeeting_http_client.h"
#include "src/net/session_manager.h"

#include <QtCore/QPointer>

#include <utility>

namespace OpenMeeting {
namespace {

HttpError localError(const QString &message) {
    HttpError error;
    error.code = static_cast<int>(ErrorCode::ParseError);
    error.message = message;
    return error;
}

} // namespace

MeetingCatalogController::Backend MeetingCatalogController::defaultBackend(SessionManager &session) {
    Backend backend;
    backend.getMeetings = [&session](const std::vector<MeetingStatus> &statuses,
                                     ResultCallback<MeetingList> callback) {
        session.httpClient().getMeetings(statuses, std::move(callback));
    };
    backend.getMeetingInfo = [&session](const QString &meetingId,
                                        ResultCallback<MeetingCatalogDetail> callback) {
        session.httpClient().getMeetingInfo(meetingId, std::move(callback));
    };
    backend.bookMeeting = [&session](const MeetingBookingRequest &request,
                                     ResultCallback<MeetingCatalogDetail> callback) {
        session.httpClient().bookMeeting(request, std::move(callback));
    };
    backend.updateMeeting = [&session](const MeetingUpdateRequest &request,
                                       ResultCallback<bool> callback) {
        session.httpClient().updateMeeting(request, std::move(callback));
    };
    backend.cancelMeeting = [&session](const QString &meetingId,
                                       ResultCallback<bool> callback) {
        session.httpClient().cancelMeeting(meetingId, std::move(callback));
    };
    return backend;
}

MeetingCatalogController::MeetingCatalogController(SessionManager &session, QObject *parent)
    : MeetingCatalogController(session, defaultBackend(session), parent) {
}

MeetingCatalogController::MeetingCatalogController(
        SessionManager &session, Backend backend, QObject *parent)
    : QObject(parent)
    , _session(session)
    , _backend(std::move(backend)) {
    connect(&_session, &SessionManager::authenticationReset, this,
            [this](quint64) { handleAccountChange(false); });
    connect(&_session, &SessionManager::loggedIn, this,
            [this](const UserInfo &) { handleAccountChange(true); });
    connect(&_session, &SessionManager::loggedOut, this,
            [this] { handleAccountChange(false); });
    connect(&_session, &SessionManager::sessionInvalidated, this,
            [this](SessionInvalidationReason) { handleAccountChange(false); });
}

std::optional<MeetingCatalogController::RequestContext>
MeetingCatalogController::captureContext() const {
    if (!_session.isLoggedIn()) return std::nullopt;
    RequestContext context;
    context.authGeneration = _session.authGeneration();
    context.httpRevision = _session.httpClient().authRevision();
    context.sessionService = _session.serverBaseUrl();
    context.httpService = _session.httpClient().baseUrl();
    context.userId = _session.userId();
    if (context.userId.isEmpty()) return std::nullopt;
    return context;
}

bool MeetingCatalogController::isContextCurrent(const RequestContext &context) const {
    return _session.isLoggedIn() && _session.authGeneration() == context.authGeneration &&
        _session.httpClient().authRevision() == context.httpRevision &&
        _session.serverBaseUrl() == context.sessionService &&
        _session.httpClient().baseUrl() == context.httpService &&
        _session.userId() == context.userId;
}

bool MeetingCatalogController::refreshUpcoming() {
    return startListQuery(false);
}

bool MeetingCatalogController::refreshHistory() {
    return startListQuery(true);
}

bool MeetingCatalogController::startListQuery(bool history) {
    auto context = captureContext();
    auto &state = history ? _history : _upcoming;
    auto &generation = history ? _historyGeneration : _upcomingGeneration;
    const auto signal = history ? &MeetingCatalogController::historyChanged
                                : &MeetingCatalogController::upcomingChanged;
    if (!context || !_backend.getMeetings) {
        state = {};
        state.state = MeetingCatalogLoadState::Error;
        state.error = localError(QStringLiteral("Meeting catalog requires an authenticated account."));
        emit (this->*signal)();
        return false;
    }

    const auto requestGeneration = ++generation;
    state.refreshing = true;
    state.error = {};
    if (!state.hasSnapshot) state.state = MeetingCatalogLoadState::Loading;
    const QPointer<MeetingCatalogController> self(this);
    emit (this->*signal)();
    if (!self || !isContextCurrent(*context) || generation != requestGeneration) return false;

    const std::vector<MeetingStatus> statuses = history
        ? std::vector<MeetingStatus>{MeetingStatus::Completed}
        : std::vector<MeetingStatus>{MeetingStatus::Scheduled, MeetingStatus::InProgress};
    auto request = _backend.getMeetings;
    request(statuses,
        [self, context = *context, requestGeneration, history](
                bool ok, const MeetingList &meetings, const HttpError &error) {
            if (!self || !self->isContextCurrent(context)) return;
            auto &currentGeneration = history ? self->_historyGeneration : self->_upcomingGeneration;
            if (currentGeneration != requestGeneration) return;
            ++currentGeneration;
            auto &current = history ? self->_history : self->_upcoming;
            current.refreshing = false;
            current.error = error;
            if (ok) {
                current.meetings = meetings;
                current.hasSnapshot = true;
                current.state = meetings.empty() ? MeetingCatalogLoadState::Empty
                                                 : MeetingCatalogLoadState::Ready;
                current.error = {};
            } else if (current.hasSnapshot) {
                current.state = current.meetings.empty() ? MeetingCatalogLoadState::Empty
                                                         : MeetingCatalogLoadState::Ready;
            } else {
                current.state = MeetingCatalogLoadState::Error;
                current.meetings.clear();
            }
            if (history) emit self->historyChanged();
            else emit self->upcomingChanged();
        });
    return true;
}

bool MeetingCatalogController::loadMeetingDetail(const QString &meetingId) {
    auto context = captureContext();
    if (meetingId.isEmpty() || !context || !_backend.getMeetingInfo) {
        _detail = {};
        _detail.meetingId = meetingId;
        _detail.state = MeetingCatalogLoadState::Error;
        _detail.error = localError(meetingId.isEmpty()
            ? QStringLiteral("Meeting ID is empty.")
            : QStringLiteral("Meeting detail requires an authenticated account."));
        emit detailChanged();
        return false;
    }

    const bool sameMeeting = _detail.meetingId == meetingId;
    const auto requestGeneration = ++_detailGeneration;
    if (!sameMeeting) _detail = {};
    _detail.meetingId = meetingId;
    _detail.refreshing = true;
    _detail.error = {};
    if (!_detail.hasSnapshot) _detail.state = MeetingCatalogLoadState::Loading;
    const QPointer<MeetingCatalogController> self(this);
    emit detailChanged();
    if (!self || !isContextCurrent(*context) || _detailGeneration != requestGeneration) return false;

    auto request = _backend.getMeetingInfo;
    request(meetingId,
        [self, context = *context, requestGeneration, meetingId](
                bool ok, const MeetingCatalogDetail &detail, const HttpError &error) {
            if (!self || !self->isContextCurrent(context) ||
                self->_detailGeneration != requestGeneration || self->_detail.meetingId != meetingId) return;
            ++self->_detailGeneration;
            self->_detail.refreshing = false;
            self->_detail.error = error;
            if (ok && detail.record.meetingId == meetingId) {
                self->_detail.detail = detail;
                self->_detail.hasSnapshot = true;
                self->_detail.state = MeetingCatalogLoadState::Ready;
                self->_detail.error = {};
            } else if (ok) {
                self->_detail.state = self->_detail.hasSnapshot
                    ? MeetingCatalogLoadState::Ready : MeetingCatalogLoadState::Error;
                self->_detail.error = localError(QStringLiteral("Meeting detail ID does not match the request."));
            } else if (self->_detail.hasSnapshot) {
                self->_detail.state = MeetingCatalogLoadState::Ready;
            } else {
                self->_detail.state = MeetingCatalogLoadState::Error;
                self->_detail.detail.reset();
            }
            emit self->detailChanged();
        });
    return true;
}

void MeetingCatalogController::clearMeetingDetail() {
    ++_detailGeneration;
    _detail = {};
    emit detailChanged();
}

void MeetingCatalogController::invalidateUpcomingQuery() {
    ++_upcomingGeneration;
    if (!_upcoming.refreshing) return;
    _upcoming.refreshing = false;
    _upcoming.state = _upcoming.hasSnapshot
        ? (_upcoming.meetings.empty() ? MeetingCatalogLoadState::Empty : MeetingCatalogLoadState::Ready)
        : MeetingCatalogLoadState::Idle;
    emit upcomingChanged();
}

void MeetingCatalogController::invalidateDetailQuery(const QString &meetingId) {
    if (_detail.meetingId != meetingId) return;
    ++_detailGeneration;
    if (!_detail.refreshing) return;
    _detail.refreshing = false;
    _detail.state = _detail.hasSnapshot ? MeetingCatalogLoadState::Ready
                                       : MeetingCatalogLoadState::Idle;
    emit detailChanged();
}

void MeetingCatalogController::publishWriteFailure(
        MeetingWriteKind kind, const QString &meetingId, const QString &message) {
    _lastWrite = {};
    _lastWrite.kind = kind;
    _lastWrite.meetingId = meetingId;
    _lastWrite.error = localError(message);
    emit writeStateChanged();
}

void MeetingCatalogController::refreshUpcomingWhenWritesSettle(const RequestContext &context) {
    if (!_upcomingRefreshPending || _bookingGeneration || !_meetingWriteGenerations.isEmpty() ||
        !isContextCurrent(context)) return;
    _upcomingRefreshPending = false;
    refreshUpcoming();
}

bool MeetingCatalogController::bookMeeting(const MeetingBookingRequest &request) {
    QString message;
    auto context = captureContext();
    if (!validateMeetingBookingRequest(request, &message) || !context ||
        !_backend.bookMeeting || _bookingGeneration) {
        if (!context) message = QStringLiteral("Meeting booking requires an authenticated account.");
        else if (_bookingGeneration) message = QStringLiteral("A meeting booking request is already in progress.");
        else if (!_backend.bookMeeting) message = QStringLiteral("Meeting booking backend is unavailable.");
        publishWriteFailure(MeetingWriteKind::Book, {}, message);
        return false;
    }

    invalidateUpcomingQuery();
    const auto generation = ++_operationGeneration;
    _bookingGeneration = generation;
    _lastWrite = {MeetingWriteKind::Book, {}, true, false, {}};
    const QPointer<MeetingCatalogController> self(this);
    emit writeStateChanged();
    if (!self || !isContextCurrent(*context) || _bookingGeneration != generation) return false;
    auto operation = _backend.bookMeeting;
    operation(request,
        [self, context = *context, generation](
                bool ok, const MeetingCatalogDetail &detail, const HttpError &error) {
            if (!self || !self->isContextCurrent(context) ||
                self->_bookingGeneration != generation) return;
            self->_bookingGeneration.reset();
            self->_lastWrite = {MeetingWriteKind::Book,
                                ok ? detail.record.meetingId : QString(), false, ok, error};
            if (ok) self->_upcomingRefreshPending = true;
            emit self->writeStateChanged();
            if (!self || !self->isContextCurrent(context)) return;
            self->refreshUpcomingWhenWritesSettle(context);
        });
    return true;
}

bool MeetingCatalogController::updateMeeting(const MeetingUpdateRequest &request) {
    QString message;
    auto context = captureContext();
    if (!validateMeetingUpdateRequest(request, &message) || !context ||
        !_backend.updateMeeting || _meetingWriteGenerations.contains(request.meetingId)) {
        if (!context) message = QStringLiteral("Meeting update requires an authenticated account.");
        else if (_meetingWriteGenerations.contains(request.meetingId)) {
            message = QStringLiteral("A write request for this meeting is already in progress.");
        } else if (!_backend.updateMeeting) message = QStringLiteral("Meeting update backend is unavailable.");
        publishWriteFailure(MeetingWriteKind::Update, request.meetingId, message);
        return false;
    }

    invalidateUpcomingQuery();
    invalidateDetailQuery(request.meetingId);
    const auto generation = ++_operationGeneration;
    _meetingWriteGenerations.insert(request.meetingId, generation);
    _lastWrite = {MeetingWriteKind::Update, request.meetingId, true, false, {}};
    const QPointer<MeetingCatalogController> self(this);
    emit writeStateChanged();
    if (!self || !isContextCurrent(*context) ||
        _meetingWriteGenerations.value(request.meetingId) != generation) return false;
    auto operation = _backend.updateMeeting;
    operation(request,
        [self, context = *context, generation, meetingId = request.meetingId](
                bool ok, const bool &, const HttpError &error) {
            if (!self || !self->isContextCurrent(context) ||
                self->_meetingWriteGenerations.value(meetingId) != generation) return;
            self->_meetingWriteGenerations.remove(meetingId);
            self->_lastWrite = {MeetingWriteKind::Update, meetingId, false, ok, error};
            if (ok) self->_upcomingRefreshPending = true;
            emit self->writeStateChanged();
            if (!self || !self->isContextCurrent(context)) return;
            if (ok && !self->_meetingWriteGenerations.contains(meetingId) &&
                self->_detail.meetingId == meetingId) {
                self->loadMeetingDetail(meetingId);
            }
            if (!self || !self->isContextCurrent(context)) return;
            self->refreshUpcomingWhenWritesSettle(context);
        });
    return true;
}

bool MeetingCatalogController::cancelMeeting(const QString &meetingId) {
    auto context = captureContext();
    if (meetingId.isEmpty() || !context || !_backend.cancelMeeting ||
        _meetingWriteGenerations.contains(meetingId)) {
        QString message;
        if (meetingId.isEmpty()) message = QStringLiteral("Meeting ID is empty.");
        else if (!context) message = QStringLiteral("Meeting cancellation requires an authenticated account.");
        else if (_meetingWriteGenerations.contains(meetingId)) {
            message = QStringLiteral("A write request for this meeting is already in progress.");
        } else message = QStringLiteral("Meeting cancellation backend is unavailable.");
        publishWriteFailure(MeetingWriteKind::Cancel, meetingId, message);
        return false;
    }

    invalidateUpcomingQuery();
    invalidateDetailQuery(meetingId);
    const auto generation = ++_operationGeneration;
    _meetingWriteGenerations.insert(meetingId, generation);
    _lastWrite = {MeetingWriteKind::Cancel, meetingId, true, false, {}};
    const QPointer<MeetingCatalogController> self(this);
    emit writeStateChanged();
    if (!self || !isContextCurrent(*context) ||
        _meetingWriteGenerations.value(meetingId) != generation) return false;
    auto operation = _backend.cancelMeeting;
    operation(meetingId,
        [self, context = *context, generation, meetingId](
                bool ok, const bool &, const HttpError &error) {
            if (!self || !self->isContextCurrent(context) ||
                self->_meetingWriteGenerations.value(meetingId) != generation) return;
            self->_meetingWriteGenerations.remove(meetingId);
            self->_lastWrite = {MeetingWriteKind::Cancel, meetingId, false, ok, error};
            if (ok) self->_upcomingRefreshPending = true;
            if (ok && self->_detail.meetingId == meetingId) {
                self->clearMeetingDetail();
                if (!self || !self->isContextCurrent(context)) return;
            }
            emit self->writeStateChanged();
            if (!self || !self->isContextCurrent(context)) return;
            self->refreshUpcomingWhenWritesSettle(context);
        });
    return true;
}

void MeetingCatalogController::handleAccountChange(bool reload) {
    ++_upcomingGeneration;
    ++_historyGeneration;
    ++_detailGeneration;
    ++_operationGeneration;
    _bookingGeneration.reset();
    _meetingWriteGenerations.clear();
    _upcomingRefreshPending = false;
    _upcoming = {};
    _history = {};
    _detail = {};
    _lastWrite = {};
    const QPointer<MeetingCatalogController> self(this);
    emit upcomingChanged();
    if (!self) return;
    emit historyChanged();
    if (!self) return;
    emit detailChanged();
    if (!self) return;
    emit writeStateChanged();
    if (!self || !reload || !_session.isLoggedIn()) return;
    refreshUpcoming();
    if (self && _session.isLoggedIn()) refreshHistory();
}

} // namespace OpenMeeting
