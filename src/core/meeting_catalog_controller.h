#pragma once

#include "src/net/http_types.h"
#include "src/net/meeting_types.h"

#include <QtCore/QHash>
#include <QtCore/QObject>

#include <functional>
#include <optional>

namespace OpenMeeting {

class SessionManager;

enum class MeetingCatalogLoadState {
    Idle,
    Loading,
    Ready,
    Empty,
    Error,
};

struct MeetingListViewState {
    MeetingCatalogLoadState state = MeetingCatalogLoadState::Idle;
    bool refreshing = false;
    bool hasSnapshot = false;
    MeetingList meetings;
    HttpError error;
};

struct MeetingDetailViewState {
    MeetingCatalogLoadState state = MeetingCatalogLoadState::Idle;
    bool refreshing = false;
    bool hasSnapshot = false;
    QString meetingId;
    std::optional<MeetingCatalogDetail> detail;
    HttpError error;
};

enum class MeetingWriteKind {
    None,
    Book,
    Update,
    Cancel,
};

struct MeetingWriteResult {
    MeetingWriteKind kind = MeetingWriteKind::None;
    QString meetingId;
    bool inFlight = false;
    bool success = false;
    HttpError error;
};

class MeetingCatalogController final : public QObject {
    Q_OBJECT
public:
    struct Backend {
        std::function<void(const std::vector<MeetingStatus> &, ResultCallback<MeetingList>)> getMeetings;
        std::function<void(const QString &, ResultCallback<MeetingCatalogDetail>)> getMeetingInfo;
        std::function<void(const MeetingBookingRequest &, ResultCallback<MeetingCatalogDetail>)> bookMeeting;
        std::function<void(const MeetingUpdateRequest &, ResultCallback<bool>)> updateMeeting;
        std::function<void(const QString &, ResultCallback<bool>)> cancelMeeting;
    };

    explicit MeetingCatalogController(SessionManager &session, QObject *parent = nullptr);
    MeetingCatalogController(SessionManager &session, Backend backend, QObject *parent = nullptr);

    const MeetingListViewState &upcomingState() const { return _upcoming; }
    const MeetingListViewState &historyState() const { return _history; }
    const MeetingDetailViewState &detailState() const { return _detail; }
    const MeetingWriteResult &lastWriteResult() const { return _lastWrite; }

    bool refreshUpcoming();
    bool refreshHistory();
    bool loadMeetingDetail(const QString &meetingId);
    void clearMeetingDetail();

    bool bookMeeting(const MeetingBookingRequest &request);
    bool updateMeeting(const MeetingUpdateRequest &request);
    bool cancelMeeting(const QString &meetingId);
    bool isBookingInFlight() const { return _bookingGeneration.has_value(); }
    bool isMeetingWriteInFlight(const QString &meetingId) const {
        return _meetingWriteGenerations.contains(meetingId);
    }

signals:
    void upcomingChanged();
    void historyChanged();
    void detailChanged();
    void writeStateChanged();

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
    bool startListQuery(bool history);
    void handleAccountChange(bool reload);
    void invalidateUpcomingQuery();
    void invalidateDetailQuery(const QString &meetingId);
    void refreshUpcomingWhenWritesSettle(const RequestContext &context);
    void publishWriteFailure(MeetingWriteKind kind, const QString &meetingId,
                             const QString &message);

    SessionManager &_session;
    Backend _backend;
    MeetingListViewState _upcoming;
    MeetingListViewState _history;
    MeetingDetailViewState _detail;
    MeetingWriteResult _lastWrite;
    quint64 _upcomingGeneration = 0;
    quint64 _historyGeneration = 0;
    quint64 _detailGeneration = 0;
    quint64 _operationGeneration = 0;
    std::optional<quint64> _bookingGeneration;
    QHash<QString, quint64> _meetingWriteGenerations;
    bool _upcomingRefreshPending = false;
};

} // namespace OpenMeeting
