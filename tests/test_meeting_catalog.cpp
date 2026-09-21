#include "src/core/meeting_catalog_controller.h"
#include "src/net/openmeeting_http_client.h"
#include "src/net/service_endpoint_policy.h"
#include "src/net/session_manager.h"
#include "src/ui/meeting_list_model.h"
#include "tests/support/test_check.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QEventLoop>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimeZone>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

using namespace OpenMeeting;

namespace OpenMeeting {
class SessionManagerTestAccess {
public:
    using Owner = std::unique_ptr<SessionManager, void (*)(SessionManager *)>;

    static Owner create(std::unique_ptr<QSettings> settings, OpenMeetingHttpClient &client) {
        return Owner(new SessionManager(std::move(settings), nullptr, &client), &destroy);
    }

private:
    static void destroy(SessionManager *session) { delete session; }
};
} // namespace OpenMeeting

namespace {

void waitFor(const std::function<bool()> &done) {
    for (int i = 0; i != 1000 && !done(); ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    TEST_CHECK(done());
}

QJsonObject detailJson(const QString &meetingId, const QString &status = QStringLiteral("Scheduled"),
                       const QJsonValue &scheduledTime = QJsonValue(QStringLiteral("1700000000"))) {
    return {
        {QStringLiteral("info"), QJsonObject{
            {QStringLiteral("systemGenerated"), QJsonObject{
                {QStringLiteral("creatorUserID"), QStringLiteral("creator-1")},
                {QStringLiteral("creatorNickname"), QStringLiteral("Creator")},
                {QStringLiteral("status"), status},
                {QStringLiteral("startTime"), QStringLiteral("1700000000")},
                {QStringLiteral("meetingID"), meetingId},
            }},
            {QStringLiteral("creatorDefinedMeeting"), QJsonObject{
                {QStringLiteral("title"), QStringLiteral("Planning")},
                {QStringLiteral("scheduledTime"), scheduledTime},
                {QStringLiteral("meetingDuration"), 3600},
                {QStringLiteral("password"), QStringLiteral("private-value")},
                {QStringLiteral("timeZone"), QStringLiteral("Asia/Shanghai")},
                {QStringLiteral("hostUserID"), QStringLiteral("creator-1")},
                {QStringLiteral("coHostUSerID"), QJsonArray{QStringLiteral("cohost-1")}},
            }},
        }},
        {QStringLiteral("setting"), QJsonObject{
            {QStringLiteral("canParticipantsEnableCamera"), true},
            {QStringLiteral("canParticipantsUnmuteMicrophone"), true},
            {QStringLiteral("canParticipantsShareScreen"), false},
            {QStringLiteral("disableCameraOnJoin"), true},
            {QStringLiteral("disableMicrophoneOnJoin"), false},
            {QStringLiteral("canParticipantJoinMeetingEarly"), true},
        }},
        {QStringLiteral("repeatInfo"), QJsonObject{
            {QStringLiteral("endDate"), QStringLiteral("0")},
            {QStringLiteral("repeatTimes"), 0},
            {QStringLiteral("repeatType"), QStringLiteral("None")},
            {QStringLiteral("uintType"), QString()},
            {QStringLiteral("interval"), 0},
            {QStringLiteral("repeatDaysOfWeek"), QJsonArray{}},
        }},
    };
}

class Server final : public QObject {
public:
    struct Request {
        QPointer<QTcpSocket> socket;
        QByteArray bytes;
        QString path;
        QByteArray token;
        QJsonObject body;
        bool parsed = false;
    };

    Server() {
        TEST_CHECK(_server.listen(QHostAddress::LocalHost, 0));
        connect(&_server, &QTcpServer::newConnection, this, [this] {
            while (_server.hasPendingConnections()) {
                auto request = std::make_shared<Request>();
                request->socket = _server.nextPendingConnection();
                connect(request->socket, &QTcpSocket::readyRead, request->socket,
                        [this, request] { receive(request); });
                receive(request);
            }
        });
    }

    QString url() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(_server.serverPort());
    }

    void received(size_t count) {
        waitFor([&] { return requests.size() >= count; });
    }

    void reply(size_t index, const QJsonValue &data, int errorCode = 0) {
        const auto payload = QJsonDocument(QJsonObject{
            {QStringLiteral("errCode"), errorCode},
            {QStringLiteral("errMsg"), errorCode ? QStringLiteral("controlled failure") : QString()},
            {QStringLiteral("data"), data},
        }).toJson(QJsonDocument::Compact);
        const auto response = QByteArray("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ") +
            QByteArray::number(payload.size()) + "\r\n\r\n" + payload;
        TEST_CHECK(index < requests.size());
        TEST_CHECK(requests[index]->socket);
        TEST_CHECK(requests[index]->socket->write(response) == response.size());
        requests[index]->socket->disconnectFromHost();
    }

    std::vector<std::shared_ptr<Request>> requests;

private:
    void receive(const std::shared_ptr<Request> &request) {
        request->bytes += request->socket->readAll();
        if (request->parsed) return;
        const int headerEnd = request->bytes.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        const auto headers = request->bytes.left(headerEnd).split('\n');
        int contentLength = -1;
        for (const auto &header : headers) {
            const int colon = header.indexOf(':');
            if (colon < 0) continue;
            const auto name = header.left(colon).trimmed().toLower();
            if (name == "content-length") contentLength = header.mid(colon + 1).trimmed().toInt();
            if (name == "token") request->token = header.mid(colon + 1).trimmed();
        }
        if (contentLength < 0 || request->bytes.size() < headerEnd + 4 + contentLength) return;
        const auto requestLine = headers.front().trimmed().split(' ');
        TEST_CHECK(requestLine.size() >= 2);
        request->path = QString::fromUtf8(requestLine[1]);
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(
            request->bytes.mid(headerEnd + 4, contentLength), &error);
        TEST_CHECK(error.error == QJsonParseError::NoError && document.isObject());
        request->body = document.object();
        request->parsed = true;
        requests.push_back(request);
    }

    QTcpServer _server;
};

void verifyTypes() {
    MeetingCatalogDetail detail;
    QString error;
    auto source = detailJson(QStringLiteral("00001234"), QStringLiteral("Future-State"),
                             QJsonValue(QStringLiteral("9223372036854775807")));
    auto repeat = source.value(QStringLiteral("repeatInfo")).toObject();
    repeat.insert(QStringLiteral("repeatType"), QStringLiteral("Weekday"));
    source.insert(QStringLiteral("repeatInfo"), repeat);
    TEST_CHECK(parseMeetingCatalogDetail(source, &detail, &error));
    TEST_CHECK(detail.record.meetingId == QStringLiteral("00001234"));
    TEST_CHECK(detail.record.scheduledTimeSeconds == std::numeric_limits<qint64>::max());
    TEST_CHECK(detail.record.status == MeetingStatus::Unknown);
    TEST_CHECK(detail.record.rawStatus == QStringLiteral("Future-State"));
    TEST_CHECK(detail.record.repeatRule.type == MeetingRepeatType::Unknown);
    TEST_CHECK(detail.record.repeatRule.rawType == QStringLiteral("Weekday"));
    TEST_CHECK(detail.password == QStringLiteral("private-value"));

    const std::vector<std::pair<MeetingRepeatType, QString>> supportedRepeatTypes{
        {MeetingRepeatType::None, QStringLiteral("None")},
        {MeetingRepeatType::Daily, QStringLiteral("Daily")},
        {MeetingRepeatType::Weekly, QStringLiteral("Weekly")},
        {MeetingRepeatType::WeekDay, QStringLiteral("WeekDay")},
        {MeetingRepeatType::Monthly, QStringLiteral("Monthly")},
    };
    for (const auto &[type, wire] : supportedRepeatTypes) {
        TEST_CHECK(isMeetingRepeatTypeSupportedForCreate(type));
        TEST_CHECK(meetingRepeatTypeToWire(type) == wire);
        TEST_CHECK(meetingRepeatTypeFromWire(wire) == type);
        MeetingRepeatRule rule;
        rule.type = type;
        rule.rawType = wire;
        TEST_CHECK(isMeetingRepeatRuleSupportedForWrite(rule, &error));
    }
    TEST_CHECK(meetingRepeatTypeFromWire(QStringLiteral("Weekday")) == MeetingRepeatType::Unknown);

    MeetingRepeatRule unsupported;
    unsupported.type = MeetingRepeatType::Custom;
    unsupported.rawType = QStringLiteral("Custom");
    unsupported.unitType = QStringLiteral("Week");
    unsupported.interval = 2;
    unsupported.daysOfWeek = {1, 3};
    TEST_CHECK(!isMeetingRepeatTypeSupportedForCreate(unsupported.type));
    TEST_CHECK(!isMeetingRepeatRuleSupportedForWrite(unsupported, &error));
    unsupported = {};
    unsupported.type = MeetingRepeatType::Unknown;
    unsupported.rawType = QStringLiteral("FutureRepeat");
    TEST_CHECK(!isMeetingRepeatRuleSupportedForWrite(unsupported, &error));

    MeetingRepeatRule dirtyFixed;
    dirtyFixed.type = MeetingRepeatType::Weekly;
    dirtyFixed.rawType = QStringLiteral("Weekly");
    dirtyFixed.interval = 1;
    TEST_CHECK(!isMeetingRepeatRuleSupportedForWrite(dirtyFixed, &error));
    dirtyFixed.interval = 0;
    dirtyFixed.repeatTimes = 5;
    TEST_CHECK(!isMeetingRepeatRuleSupportedForWrite(dirtyFixed, &error));
    MeetingRepeatRule nonRepeatingWithEnd;
    nonRepeatingWithEnd.endDateSeconds = 1700000000;
    TEST_CHECK(!isMeetingRepeatRuleSupportedForWrite(nonRepeatingWithEnd, &error));

    MeetingBookingRequest datedRepeat;
    datedRepeat.title = QStringLiteral("Dated repeat");
    datedRepeat.scheduledTimeSeconds = 1700000000;
    datedRepeat.meetingDurationSeconds = 3600;
    datedRepeat.timeZone = QStringLiteral("Asia/Shanghai");
    datedRepeat.repeatRule.type = MeetingRepeatType::Monthly;
    datedRepeat.repeatRule.rawType = QStringLiteral("Monthly");
    datedRepeat.repeatRule.endDateSeconds = datedRepeat.scheduledTimeSeconds - 1;
    TEST_CHECK(!validateMeetingBookingRequest(datedRepeat, &error));
    datedRepeat.repeatRule.endDateSeconds = datedRepeat.scheduledTimeSeconds;
    TEST_CHECK(validateMeetingBookingRequest(datedRepeat, &error));

    auto fixedSource = detailJson(QStringLiteral("repeat-1"));
    auto fixedRepeat = fixedSource.value(QStringLiteral("repeatInfo")).toObject();
    fixedRepeat.insert(QStringLiteral("repeatType"), QStringLiteral("WeekDay"));
    fixedRepeat.insert(QStringLiteral("endDate"), QStringLiteral("1700600000"));
    fixedSource.insert(QStringLiteral("repeatInfo"), fixedRepeat);
    TEST_CHECK(parseMeetingCatalogDetail(fixedSource, &detail, &error));
    TEST_CHECK(detail.record.repeatRule.type == MeetingRepeatType::WeekDay);
    TEST_CHECK(detail.record.repeatRule.endDateSeconds == 1700600000);

    auto unsafe = detailJson(QStringLiteral("1"), QStringLiteral("Scheduled"),
                             QJsonValue(9007199254740992.0));
    TEST_CHECK(!parseMeetingCatalogDetail(unsafe, &detail, &error));
    TEST_CHECK(!error.isEmpty());

    MeetingList list{{}};
    TEST_CHECK(parseMeetingList(QJsonValue(QJsonValue::Null), &list, &error));
    TEST_CHECK(list.empty());
    TEST_CHECK(!parseMeetingList(QJsonObject{}, &list, &error));

    MeetingUpdateRequest update;
    update.meetingId = QStringLiteral("meeting-1");
    update.password = QString();
    update.disableCameraOnJoin = false;
    const auto body = meetingUpdateToJson(update, QStringLiteral("user-1"));
    TEST_CHECK(body.contains(QStringLiteral("password")) && body.value(QStringLiteral("password")).toString().isEmpty());
    TEST_CHECK(body.contains(QStringLiteral("disableCameraOnJoin")) && !body.value(QStringLiteral("disableCameraOnJoin")).toBool());
    TEST_CHECK(!body.contains(QStringLiteral("title")));
    std::puts("MEETING TYPES PASS: repeat write boundary, wire values, nested schema, int64, PATCH presence");
}

MeetingBookingRequest bookingRequest() {
    MeetingBookingRequest request;
    request.title = QStringLiteral("Planning");
    request.scheduledTimeSeconds = 1700000000;
    request.meetingDurationSeconds = 3600;
    request.password = QStringLiteral("private-value");
    request.timeZone = QStringLiteral("Asia/Shanghai");
    request.settings.canParticipantsEnableCamera = true;
    request.settings.canParticipantsUnmuteMicrophone = true;
    request.settings.disableCameraOnJoin = true;
    request.repeatRule.type = MeetingRepeatType::Daily;
    request.repeatRule.rawType = QStringLiteral("Daily");
    request.repeatRule.endDateSeconds = 1700600000;
    return request;
}

void verifyHttpContracts() {
    Server server;
    OpenMeetingHttpClient client;
    client.setBaseUrl(server.url());
    client.setCurrentUser(UserInfo{QStringLiteral("catalog-token"), QStringLiteral("user-1"),
                                   QStringLiteral("User One"), {}});

    int callbacks = 0;
    client.bookMeeting(bookingRequest(), [&](bool ok, const MeetingCatalogDetail &detail, const HttpError &) {
        TEST_CHECK(ok && detail.record.meetingId == QStringLiteral("00001234"));
        ++callbacks;
    });
    server.received(1);
    TEST_CHECK(server.requests[0]->path == QStringLiteral("/meeting/book_meeting"));
    TEST_CHECK(server.requests[0]->token == QByteArray("catalog-token"));
    const auto bookBody = server.requests[0]->body;
    TEST_CHECK(bookBody.value(QStringLiteral("creatorUserID")) == QStringLiteral("user-1"));
    TEST_CHECK(bookBody.value(QStringLiteral("creatorDefinedMeetingInfo")).toObject()
        .value(QStringLiteral("scheduledTime")).toDouble() == 1700000000.0);
    const auto bookRepeat = bookBody.value(QStringLiteral("repeatInfo")).toObject();
    TEST_CHECK(bookRepeat.value(QStringLiteral("repeatType")) == QStringLiteral("Daily"));
    TEST_CHECK(bookRepeat.value(QStringLiteral("endDate")).toDouble() == 1700600000.0);
    TEST_CHECK(bookRepeat.value(QStringLiteral("repeatTimes")).toInt(-1) == 0);
    TEST_CHECK(bookRepeat.value(QStringLiteral("uintType")).toString().isEmpty());
    TEST_CHECK(bookRepeat.value(QStringLiteral("interval")).toInt(-1) == 0);
    TEST_CHECK(bookRepeat.value(QStringLiteral("repeatDaysOfWeek")).toArray().isEmpty());
    server.reply(0, QJsonObject{{QStringLiteral("detail"), detailJson(QStringLiteral("00001234"))}});
    waitFor([&] { return callbacks == 1; });

    MeetingUpdateRequest update;
    update.meetingId = QStringLiteral("00001234");
    update.password = QString();
    update.disableCameraOnJoin = false;
    update.canParticipantsShareScreen = false;
    update.repeatRule = MeetingRepeatRule{};
    client.updateMeeting(update, [&](bool ok, const bool &value, const HttpError &) {
        TEST_CHECK(ok && value);
        ++callbacks;
    });
    server.received(2);
    TEST_CHECK(server.requests[1]->path == QStringLiteral("/meeting/update_meeting"));
    const auto updateBody = server.requests[1]->body;
    TEST_CHECK(updateBody.value(QStringLiteral("updatingUserID")) == QStringLiteral("user-1"));
    TEST_CHECK(updateBody.contains(QStringLiteral("password")) &&
               updateBody.value(QStringLiteral("password")).toString().isEmpty());
    TEST_CHECK(updateBody.contains(QStringLiteral("disableCameraOnJoin")) &&
               !updateBody.value(QStringLiteral("disableCameraOnJoin")).toBool());
    TEST_CHECK(!updateBody.contains(QStringLiteral("title")));
    TEST_CHECK(updateBody.value(QStringLiteral("repeatInfo")).toObject().contains(QStringLiteral("endDate")));
    server.reply(1, QJsonObject{});
    waitFor([&] { return callbacks == 2; });

    client.cancelMeeting(QStringLiteral("00001234"), [&](bool ok, const bool &, const HttpError &) {
        TEST_CHECK(ok);
        ++callbacks;
    });
    server.received(3);
    TEST_CHECK(server.requests[2]->path == QStringLiteral("/meeting/end_meeting"));
    TEST_CHECK(server.requests[2]->body.value(QStringLiteral("endType")).toInt(-1) == 0);
    server.reply(2, QJsonObject{});
    waitFor([&] { return callbacks == 3; });

    client.getMeetings({MeetingStatus::Scheduled, MeetingStatus::InProgress},
        [&](bool ok, const MeetingList &meetings, const HttpError &) {
            TEST_CHECK(ok && meetings.size() == 1 && meetings.front().meetingId == QStringLiteral("00001234"));
            ++callbacks;
        });
    server.received(4);
    TEST_CHECK(server.requests[3]->path == QStringLiteral("/meeting/get_meetings"));
    const auto statuses = server.requests[3]->body.value(QStringLiteral("status")).toArray();
    const QJsonArray expectedStatuses{QStringLiteral("Scheduled"), QStringLiteral("In-Progress")};
    TEST_CHECK(statuses == expectedStatuses);
    server.reply(3, QJsonObject{{QStringLiteral("meetingDetails"),
                                QJsonArray{detailJson(QStringLiteral("00001234"))}}});
    waitFor([&] { return callbacks == 4; });

    client.getMeetingInfo(QStringLiteral("00001234"),
        [&](bool ok, const MeetingCatalogDetail &detail, const HttpError &) {
            TEST_CHECK(ok && detail.record.meetingId == QStringLiteral("00001234"));
            ++callbacks;
        });
    server.received(5);
    TEST_CHECK(server.requests[4]->path == QStringLiteral("/meeting/get_meeting"));
    TEST_CHECK(server.requests[4]->body.value(QStringLiteral("meetingID")) == QStringLiteral("00001234"));
    server.reply(4, QJsonObject{{QStringLiteral("meetingDetail"), detailJson(QStringLiteral("00001234"))}});
    waitFor([&] { return callbacks == 5; });

    client.getMeetings({MeetingStatus::Completed},
        [&](bool ok, const MeetingList &, const HttpError &error) {
            TEST_CHECK(!ok && error.code == static_cast<int>(ErrorCode::ParseError));
            ++callbacks;
        });
    server.received(6);
    server.reply(5, QJsonObject{{QStringLiteral("meetingDetails"), QStringLiteral("invalid")}});
    waitFor([&] { return callbacks == 6; });
    std::puts("MEETING HTTP PASS: typed paths, identity injection, request shape, cancellation, parse errors");
}

struct ControlledBackend {
    struct ListCall {
        std::vector<MeetingStatus> statuses;
        ResultCallback<MeetingList> callback;
    };
    struct DetailCall { QString meetingId; ResultCallback<MeetingCatalogDetail> callback; };
    struct BookCall { MeetingBookingRequest request; ResultCallback<MeetingCatalogDetail> callback; };
    struct UpdateCall { MeetingUpdateRequest request; ResultCallback<bool> callback; };
    struct CancelCall { QString meetingId; ResultCallback<bool> callback; };

    MeetingCatalogController::Backend backend() {
        MeetingCatalogController::Backend result;
        result.getMeetings = [this](const std::vector<MeetingStatus> &statuses,
                                    ResultCallback<MeetingList> callback) {
            lists.push_back({statuses, std::move(callback)});
        };
        result.getMeetingInfo = [this](const QString &id, ResultCallback<MeetingCatalogDetail> callback) {
            details.push_back({id, std::move(callback)});
        };
        result.bookMeeting = [this](const MeetingBookingRequest &request,
                                    ResultCallback<MeetingCatalogDetail> callback) {
            books.push_back({request, std::move(callback)});
        };
        result.updateMeeting = [this](const MeetingUpdateRequest &request,
                                      ResultCallback<bool> callback) {
            updates.push_back({request, std::move(callback)});
        };
        result.cancelMeeting = [this](const QString &id, ResultCallback<bool> callback) {
            cancellations.push_back({id, std::move(callback)});
        };
        return result;
    }

    std::vector<ListCall> lists;
    std::vector<DetailCall> details;
    std::vector<BookCall> books;
    std::vector<UpdateCall> updates;
    std::vector<CancelCall> cancellations;
};

MeetingCatalogDetail controlledDetail(
        const QString &meetingId, const QString &title = QStringLiteral("Controlled")) {
    MeetingCatalogDetail detail;
    detail.record.meetingId = meetingId;
    detail.record.title = title;
    detail.record.status = MeetingStatus::Scheduled;
    detail.record.rawStatus = QStringLiteral("Scheduled");
    return detail;
}

void verifyController() {
    QTemporaryDir directory;
    TEST_CHECK(directory.isValid());
    const auto settingsPath = directory.filePath(QStringLiteral("catalog.ini"));
    auto settings = std::make_unique<QSettings>(settingsPath, QSettings::IniFormat);
    settings->setFallbacksEnabled(false);
    settings->setValue(QStringLiteral("network/serverBaseUrl"), QStringLiteral("http://127.0.0.1:1"));
    settings->sync();
    OpenMeetingHttpClient client;
    auto session = SessionManagerTestAccess::create(std::move(settings), client);
    session->loginAsGuest(QStringLiteral("First"), QStringLiteral("first-user"));

    ControlledBackend controlled;
    MeetingCatalogController controller(*session, controlled.backend());
    int upcomingSignals = 0;
    int detailSignals = 0;
    QObject::connect(&controller, &MeetingCatalogController::upcomingChanged,
                     &controller, [&] { ++upcomingSignals; });
    QObject::connect(&controller, &MeetingCatalogController::detailChanged,
                     &controller, [&] { ++detailSignals; });

    TEST_CHECK(controller.refreshUpcoming());
    TEST_CHECK(controller.refreshUpcoming());
    TEST_CHECK(controlled.lists.size() == 2);
    MeetingList newest{controlledDetail(QStringLiteral("new")).record};
    MeetingList stale{controlledDetail(QStringLiteral("old")).record};
    const auto duplicate = controlled.lists[1].callback;
    controlled.lists[1].callback(true, newest, {});
    const int afterNewest = upcomingSignals;
    controlled.lists[0].callback(true, stale, {});
    duplicate(true, stale, {});
    TEST_CHECK(upcomingSignals == afterNewest);
    TEST_CHECK(controller.upcomingState().meetings.front().meetingId == QStringLiteral("new"));

    TEST_CHECK(controller.refreshUpcoming());
    TEST_CHECK(controlled.lists.size() == 3);
    controlled.lists[2].callback(false, {}, HttpError{77, QStringLiteral("refresh failed"), {}});
    TEST_CHECK(controller.upcomingState().state == MeetingCatalogLoadState::Ready);
    TEST_CHECK(controller.upcomingState().meetings.front().meetingId == QStringLiteral("new"));
    TEST_CHECK(controller.upcomingState().error.code == 77);

    TEST_CHECK(controller.loadMeetingDetail(QStringLiteral("A")));
    TEST_CHECK(controller.loadMeetingDetail(QStringLiteral("B")));
    TEST_CHECK(controlled.details.size() == 2);
    controlled.details[0].callback(true, controlledDetail(QStringLiteral("A")), {});
    TEST_CHECK(controller.detailState().meetingId == QStringLiteral("B"));
    controlled.details[1].callback(true, controlledDetail(QStringLiteral("B")), {});
    TEST_CHECK(controller.detailState().detail->record.meetingId == QStringLiteral("B"));

    MeetingUpdateRequest update;
    update.meetingId = QStringLiteral("B");
    update.disableCameraOnJoin = false;
    TEST_CHECK(controller.updateMeeting(update));
    TEST_CHECK(!controller.cancelMeeting(QStringLiteral("B")));
    TEST_CHECK(controller.isMeetingWriteInFlight(QStringLiteral("B")));
    TEST_CHECK(controlled.updates.size() == 1);
    controlled.updates[0].callback(true, true, {});
    TEST_CHECK(!controller.isMeetingWriteInFlight(QStringLiteral("B")));
    TEST_CHECK(controlled.details.size() == 3);
    TEST_CHECK(controlled.lists.size() == 4);
    controlled.details[2].callback(true, controlledDetail(QStringLiteral("B"), QStringLiteral("Updated")), {});
    controlled.lists[3].callback(true, newest, {});
    TEST_CHECK(controller.detailState().detail->record.title == QStringLiteral("Updated"));

    TEST_CHECK(controller.bookMeeting(bookingRequest()));
    TEST_CHECK(!controller.bookMeeting(bookingRequest()));
    TEST_CHECK(controlled.books.size() == 1);
    controlled.books[0].callback(true, controlledDetail(QStringLiteral("booked")), {});
    TEST_CHECK(controlled.lists.size() == 5);
    controlled.lists[4].callback(true, MeetingList{controlledDetail(QStringLiteral("booked")).record}, {});
    TEST_CHECK(controller.lastWriteResult().success);
    TEST_CHECK(controller.lastWriteResult().meetingId == QStringLiteral("booked"));

    TEST_CHECK(controller.refreshUpcoming());
    const size_t staleAccountQuery = controlled.lists.size() - 1;
    session->loginAsGuest(QStringLiteral("Second"), QStringLiteral("second-user"));
    TEST_CHECK(controlled.lists.size() >= staleAccountQuery + 3);
    const size_t newUpcomingQuery = staleAccountQuery + 1;
    const size_t newHistoryQuery = staleAccountQuery + 2;
    controlled.lists[staleAccountQuery].callback(true, stale, {});
    TEST_CHECK(controller.upcomingState().meetings.empty());
    controlled.lists[newUpcomingQuery].callback(true, newest, {});
    controlled.lists[newHistoryQuery].callback(true, {}, {});
    TEST_CHECK(controller.upcomingState().meetings.front().meetingId == QStringLiteral("new"));

    auto disposable = std::make_unique<MeetingCatalogController>(*session, controlled.backend());
    TEST_CHECK(disposable->refreshUpcoming());
    const auto afterDestroy = controlled.lists.back().callback;
    disposable.reset();
    afterDestroy(true, stale, {});
    QCoreApplication::processEvents();
    TEST_CHECK(detailSignals > 0);

    TEST_CHECK(session->setServerBaseUrl(QStringLiteral("http://127.0.0.1:2")));
    TEST_CHECK(!session->isLoggedIn());
    TEST_CHECK(controller.upcomingState().state == MeetingCatalogLoadState::Idle);
    TEST_CHECK(controller.upcomingState().meetings.empty());
    std::puts("MEETING CATALOG PASS: stale/duplicate callbacks, snapshots, write serialization, account and lifetime guards");
}

void verifyMeetingListProjection() {
    MeetingUI::MeetingListModel model;
    MeetingRecord later;
    later.meetingId = QStringLiteral("000000002");
    later.title = QStringLiteral("Design Review");
    later.creatorUserId = QStringLiteral("current-user");
    later.creatorNickname = QStringLiteral("Alice");
    later.status = MeetingStatus::Scheduled;
    later.scheduledTimeSeconds = 1700003600;
    later.timeZone = QStringLiteral("Asia/Shanghai");

    const QTimeZone shanghai("Asia/Shanghai");
    later.repeatRule.type = MeetingRepeatType::Daily;
    later.repeatRule.rawType = QStringLiteral("Daily");
    later.repeatRule.endDateSeconds = QDateTime(
        QDate(2026, 10, 31), QTime(9, 0), shanghai).toSecsSinceEpoch();

    MeetingRecord earlier = later;
    earlier.meetingId = QStringLiteral("000000001");
    earlier.title = QStringLiteral("Planning");
    earlier.scheduledTimeSeconds = 1700000000;

    MeetingRecord duplicate = earlier;
    duplicate.title = QStringLiteral("Duplicate must not be projected");

    model.setMeetings({later, earlier, duplicate}, false);
    TEST_CHECK(model.rowCount() == 2);
    TEST_CHECK(model.index(0, 0).data(MeetingUI::MeetingIdRole).toString() ==
               QStringLiteral("000000001"));
    TEST_CHECK(model.index(1, 0).data(MeetingUI::MeetingIdRole).toString() ==
               QStringLiteral("000000002"));
    TEST_CHECK(model.index(1, 0).data(MeetingUI::MeetingRepeatRole).toString() ==
               QString::fromUtf8("Repeats Daily, until 2026-10-31"));
    TEST_CHECK(model.index(0, 0).data(MeetingUI::MeetingShowDateHeaderRole).toBool());
    TEST_CHECK(!model.index(1, 0).data(MeetingUI::MeetingShowDateHeaderRole).toBool());

    model.setSearchText(QStringLiteral("alice"));
    TEST_CHECK(model.rowCount() == 2);
    model.setSearchText(QStringLiteral("planning"));
    TEST_CHECK(model.rowCount() == 1);
    TEST_CHECK(model.index(0, 0).data(MeetingUI::MeetingIdRole).toString() ==
               QStringLiteral("000000001"));

    MeetingRecord otherCreator = later;
    otherCreator.meetingId = QStringLiteral("000000003");
    otherCreator.creatorUserId = QStringLiteral("other-user");
    otherCreator.scheduledTimeSeconds = 1700007200;
    model.setSearchText({});
    model.setMeetings({earlier, later, otherCreator}, true, QStringLiteral("current-user"));
    TEST_CHECK(model.rowCount() == 2);
    TEST_CHECK(model.index(0, 0).data(MeetingUI::MeetingIdRole).toString() ==
               QStringLiteral("000000002"));
    TEST_CHECK(model.index(1, 0).data(MeetingUI::MeetingIdRole).toString() ==
               QStringLiteral("000000001"));

    MeetingRepeatRule custom;
    custom.type = MeetingRepeatType::Custom;
    custom.rawType = QStringLiteral("Custom");
    custom.unitType = QStringLiteral("Week");
    custom.interval = 2;
    custom.daysOfWeek = {1, 3};
    const auto customText = MeetingUI::MeetingListModel::repeatText(custom, QStringLiteral("Asia/Shanghai"));
    TEST_CHECK(customText.contains(QString::fromUtf8("Custom Recurrence (Read-Only")));
    TEST_CHECK(customText.contains(QString::fromUtf8("Every 2 week(s)")));
    TEST_CHECK(customText.contains(QString::fromUtf8("Mon, Wed")));

    MeetingRepeatRule unknown;
    unknown.type = MeetingRepeatType::Unknown;
    unknown.rawType = QStringLiteral("FutureRepeat");
    TEST_CHECK(MeetingUI::MeetingListModel::repeatText(unknown).contains(
        QString::fromUtf8("FutureRepeat (Read-Only)")));
    std::puts("MEETING LIST MODEL PASS: deduplication, ordering, grouping, search, creator history boundary");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    initializeServiceEndpointPolicy(true);
    verifyTypes();
    verifyHttpContracts();
    verifyController();
    verifyMeetingListProjection();
    std::puts("MEETING CATALOG TESTS PASSED");
    return 0;
}
