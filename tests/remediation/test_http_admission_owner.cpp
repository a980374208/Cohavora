#include "src/core/meeting_coordinator.h"
#include "src/net/service_endpoint_policy.h"
#include "tests/support/test_check.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEvent>
#include <QtCore/QJsonDocument>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <functional>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace OpenMeeting {

class SessionManagerTestAccess final {
public:
    using ScopedSession = std::unique_ptr<SessionManager, void (*)(SessionManager *)>;

    static ScopedSession create(std::unique_ptr<QSettings> settings) {
        return ScopedSession(new SessionManager(std::move(settings)), &destroy);
    }

private:
    static void destroy(SessionManager *session) { delete session; }
};

class MeetingCoordinatorTestAccess final {
public:
    struct Backend {
        std::function<void(const QString &, const QString &, ResultCallback<bool>)> joinMeeting;
        std::function<void(const QString &, ResultCallback<LiveKitAuthInfo>)> getMeetingToken;
        std::function<void(const QString &, int, ResultCallback<LiveKitAuthInfo>)> createImmediateMeeting;
        std::function<void(const QString &, ResultCallback<bool>)> leaveMeeting;
        std::function<void(const QString &, ResultCallback<bool>)> endMeeting;
    };

    static std::unique_ptr<MeetingCoordinator> create(SessionManager &session, Backend backend) {
        MeetingCoordinator::AdmissionBackend productionBackend{
            std::move(backend.joinMeeting), std::move(backend.getMeetingToken),
            std::move(backend.createImmediateMeeting), std::move(backend.leaveMeeting),
            std::move(backend.endMeeting),
        };
        return std::unique_ptr<MeetingCoordinator>(
            new MeetingCoordinator(session, std::move(productionBackend), nullptr));
    }

    static std::unique_ptr<MeetingCoordinator> createDefault(SessionManager &session) {
        return std::unique_ptr<MeetingCoordinator>(new MeetingCoordinator(
            session, MeetingCoordinator::makeDefaultAdmissionBackend(session), nullptr));
    }

    static void setRoomStartHook(MeetingCoordinator &coordinator,
                                 std::function<void(const QString &, const QString &)> hook) {
        coordinator._roomStartHook = std::move(hook);
    }

    static bool hasRoomArtifacts(const MeetingCoordinator &coordinator) {
        return coordinator._sessionRunning.load() || coordinator._ioContext ||
               coordinator._sessionRuntime || coordinator._room ||
               coordinator._roomListener || coordinator._ioThread.joinable();
    }

    static void markRoomSessionRunningForCleanup(MeetingCoordinator &coordinator) {
        coordinator._sessionRunning.store(true);
    }

    static void duplicateIdentityKick(MeetingCoordinator &coordinator, const QString &detail) {
        coordinator.handleDuplicateIdentityKickOff(detail);
    }
};

} // namespace OpenMeeting

namespace {

using OpenMeeting::HttpError;
using OpenMeeting::LiveKitAuthInfo;
using OpenMeeting::MeetingCoordinator;
using OpenMeeting::MeetingCoordinatorTestAccess;
using OpenMeeting::MeetingDetail;
using OpenMeeting::MeetingState;
using OpenMeeting::MediaPreferences;
using OpenMeeting::ResultCallback;
using OpenMeeting::SessionInvalidationReason;
using OpenMeeting::SessionManager;
using OpenMeeting::SessionManagerTestAccess;

constexpr int kPlannedCases = 75;
int gExecutedCases = 0;
int gPassedCases = 0;

template <typename Function>
void RunCase(const QString &name, Function &&function) {
    ++gExecutedCases;
    std::cout << "[CASE " << gExecutedCases << "] " << name.toStdString() << std::endl;
    function();
    ++gPassedCases;
}

void DrainEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}

template <typename Predicate>
void WaitUntil(Predicate &&predicate, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(1);
    }
    TEST_CHECK(predicate());
}

std::unique_ptr<QSettings> MakeSettings(const QString &directory, const QString &baseUrl) {
    const QString path = QDir(directory).filePath("cppqt001/session.ini");
    auto settings = std::make_unique<QSettings>(path, QSettings::IniFormat);
    settings->setFallbacksEnabled(false);
    TEST_CHECK(settings->format() == QSettings::IniFormat);
    TEST_CHECK(QDir::cleanPath(settings->fileName()) == QDir::cleanPath(path));
    TEST_CHECK(QDir::cleanPath(path).startsWith(QDir::cleanPath(directory)));
    settings->setValue("network/serverBaseUrl", baseUrl);
    settings->sync();
    TEST_CHECK(settings->status() == QSettings::NoError);
    return settings;
}

struct BoolRequest {
    QString meetingId;
    QString password;
    ResultCallback<bool> completion;
};

struct AuthRequest {
    QString value;
    int durationSeconds = 0;
    ResultCallback<LiveKitAuthInfo> completion;
};

class FakeAdmissionBackend final {
public:
    MeetingCoordinatorTestAccess::Backend functions() {
        return {
            [this](const QString &meetingId, const QString &password, ResultCallback<bool> callback) {
                ++dispatches;
                if (synchronousJoin) {
                    synchronousJoin(std::move(callback));
                } else {
                    joins.push_back({meetingId, password, std::move(callback)});
                }
            },
            [this](const QString &meetingId, ResultCallback<LiveKitAuthInfo> callback) {
                ++dispatches;
                if (synchronousToken) {
                    synchronousToken(std::move(callback));
                } else {
                    tokens.push_back({meetingId, 0, std::move(callback)});
                }
            },
            [this](const QString &title, int durationSeconds, ResultCallback<LiveKitAuthInfo> callback) {
                ++dispatches;
                if (synchronousCreate) {
                    synchronousCreate(std::move(callback));
                } else {
                    creates.push_back({title, durationSeconds, std::move(callback)});
                }
            },
            [this](const QString &meetingId, ResultCallback<bool> callback) {
                ++dispatches;
                leaves.push_back(meetingId);
                leaveCompletion = std::move(callback);
            },
            [this](const QString &meetingId, ResultCallback<bool> callback) {
                ++dispatches;
                ends.push_back(meetingId);
                endCompletion = std::move(callback);
            },
        };
    }

    void completeJoin(size_t index, bool ok, const QString &message = QString()) {
        TEST_CHECK(index < joins.size());
        const auto completion = joins[index].completion;
        TEST_CHECK(static_cast<bool>(completion));
        HttpError error;
        error.message = message;
        ++deliveries;
        completion(ok, ok, error);
    }

    void completeToken(size_t index, bool ok, const QString &meetingId,
                       const QString &url = QStringLiteral("wss://fixture.invalid"),
                       const QString &token = QStringLiteral("fixture-token"),
                       const QString &message = QString()) {
        TEST_CHECK(index < tokens.size());
        const auto completion = tokens[index].completion;
        TEST_CHECK(static_cast<bool>(completion));
        const LiveKitAuthInfo auth{url, token, meetingId};
        HttpError error;
        error.message = message;
        ++deliveries;
        completion(ok, auth, error);
    }

    void completeCreate(size_t index, bool ok, const QString &meetingId,
                        const QString &url = QStringLiteral("wss://fixture.invalid"),
                        const QString &token = QStringLiteral("fixture-token"),
                        const QString &message = QString()) {
        TEST_CHECK(index < creates.size());
        const auto completion = creates[index].completion;
        TEST_CHECK(static_cast<bool>(completion));
        const LiveKitAuthInfo auth{url, token, meetingId};
        HttpError error;
        error.message = message;
        ++deliveries;
        completion(ok, auth, error);
    }

    int dispatches = 0;
    int deliveries = 0;
    std::vector<BoolRequest> joins;
    std::vector<AuthRequest> tokens;
    std::vector<AuthRequest> creates;
    std::vector<QString> leaves;
    std::vector<QString> ends;
    ResultCallback<bool> leaveCompletion;
    ResultCallback<bool> endCompletion;
    std::function<void(ResultCallback<bool>)> synchronousJoin;
    std::function<void(ResultCallback<LiveKitAuthInfo>)> synchronousToken;
    std::function<void(ResultCallback<LiveKitAuthInfo>)> synchronousCreate;
};

struct ErrorRecord {
    QString title;
    QString message;
};

class Fixture final {
public:
    Fixture()
        : session(SessionManagerTestAccess::create(
              MakeSettings(settingsDirectory.path(), QStringLiteral("http://127.0.0.1:1")))) {
        TEST_CHECK(settingsDirectory.isValid());
        session->loginAsGuest(QStringLiteral("Fixture User"), QStringLiteral("fixture-user"));
        TEST_CHECK(session->isLoggedIn());
        coordinator = MeetingCoordinatorTestAccess::create(*session, backend.functions());
        installObservers();
    }

    ~Fixture() {
        coordinator.reset();
        DrainEvents();
        session->logout(false);
        DrainEvents();
    }

    void installObservers() {
        MeetingCoordinatorTestAccess::setRoomStartHook(
            *coordinator, [this](const QString &url, const QString &token) {
                TEST_CHECK(QThread::currentThread() == coordinator->thread());
                ++starts;
                startedUrls.push_back(url);
                startedTokens.push_back(token);
                TEST_CHECK(!MeetingCoordinatorTestAccess::hasRoomArtifacts(*coordinator));
            });
        QObject::connect(coordinator.get(), &MeetingCoordinator::stateChanged,
                         coordinator.get(), [this](MeetingState state, const QString &) {
                             TEST_CHECK(QThread::currentThread() == coordinator->thread());
                             states.push_back(state);
                         });
        QObject::connect(coordinator.get(), &MeetingCoordinator::errorOccurred,
                         coordinator.get(), [this](const QString &title, const QString &message) {
                             errors.push_back({title, message});
                         });
        QObject::connect(coordinator.get(), &MeetingCoordinator::meetingDetailUpdated,
                         coordinator.get(), [this](const MeetingDetail &detail) {
                             details.push_back(detail);
                         });
        QObject::connect(coordinator.get(), &MeetingCoordinator::meetingLeft,
                         coordinator.get(), [this]() { ++meetingLeftCount; });
        QObject::connect(coordinator.get(), &MeetingCoordinator::meetingKickOff,
                         coordinator.get(), [this](livekit::RoomDisconnectReason) { ++kickCount; });
    }

    QTemporaryDir settingsDirectory;
    SessionManagerTestAccess::ScopedSession session;
    FakeAdmissionBackend backend;
    std::unique_ptr<MeetingCoordinator> coordinator;
    std::vector<MeetingState> states;
    std::vector<ErrorRecord> errors;
    std::vector<MeetingDetail> details;
    std::vector<QString> startedUrls;
    std::vector<QString> startedTokens;
    int starts = 0;
    int meetingLeftCount = 0;
    int kickCount = 0;
};

enum class PendingStage { Join, Token, Create };

QString StageName(PendingStage stage) {
    switch (stage) {
        case PendingStage::Join: return QStringLiteral("Join");
        case PendingStage::Token: return QStringLiteral("Token");
        case PendingStage::Create: return QStringLiteral("Create");
    }
    return QStringLiteral("Unknown");
}

size_t PreparePending(Fixture &fixture, PendingStage stage, const QString &id) {
    MediaPreferences preferences;
    if (stage == PendingStage::Create) {
        const size_t index = fixture.backend.creates.size();
        fixture.coordinator->createAndJoinQuickMeetingAsync(id + "-title", 900, preferences);
        TEST_CHECK(fixture.backend.creates.size() == index + 1);
        return index;
    }
    const size_t joinIndex = fixture.backend.joins.size();
    fixture.coordinator->joinMeetingAsync(id, id + "-password", id + "-name", preferences);
    TEST_CHECK(fixture.backend.joins.size() == joinIndex + 1);
    if (stage == PendingStage::Join) return joinIndex;
    const size_t tokenIndex = fixture.backend.tokens.size();
    fixture.backend.completeJoin(joinIndex, true);
    TEST_CHECK(fixture.backend.tokens.size() == tokenIndex + 1);
    return tokenIndex;
}

void DeliverPending(Fixture &fixture, PendingStage stage, size_t index,
                    bool success, const QString &id) {
    if (stage == PendingStage::Join) {
        fixture.backend.completeJoin(index, success,
                                     success ? QString() : QStringLiteral("old-join-error"));
    } else if (stage == PendingStage::Token) {
        fixture.backend.completeToken(index, success, id, QStringLiteral("wss://") + id,
                                      id + "-token",
                                      success ? QString() : QStringLiteral("old-token-error"));
    } else {
        fixture.backend.completeCreate(index, success, id, QStringLiteral("wss://") + id,
                                       id + "-token",
                                       success ? QString() : QStringLiteral("old-create-error"));
    }
}

void CompleteCurrent(Fixture &fixture, PendingStage stage, size_t index, const QString &id) {
    DeliverPending(fixture, stage, index, true, id);
    if (stage == PendingStage::Join) {
        TEST_CHECK(!fixture.backend.tokens.empty());
        fixture.backend.completeToken(fixture.backend.tokens.size() - 1, true, id,
                                      QStringLiteral("wss://") + id, id + "-token");
    }
}

void VerifyNormalFlows() {
    RunCase("normal Join -> Token", [] {
        Fixture fixture;
        const auto index = PreparePending(fixture, PendingStage::Join, "normal-join");
        CompleteCurrent(fixture, PendingStage::Join, index, "normal-join");
        TEST_CHECK(fixture.starts == 1);
        TEST_CHECK(fixture.startedUrls.back() == "wss://normal-join");
        TEST_CHECK(fixture.coordinator->state() == MeetingState::ConnectingRoom);
        TEST_CHECK(fixture.errors.empty());
    });
    RunCase("normal Quick", [] {
        Fixture fixture;
        const auto index = PreparePending(fixture, PendingStage::Create, "normal-quick");
        CompleteCurrent(fixture, PendingStage::Create, index, "quick-id");
        TEST_CHECK(fixture.starts == 1);
        TEST_CHECK(fixture.details.size() == 1);
        TEST_CHECK(fixture.details.back().meetingId == "quick-id");
        TEST_CHECK(fixture.details.back().meetingName == "normal-quick-title");
        TEST_CHECK(fixture.details.back().hostUserId == "fixture-user");
    });
    RunCase("normal Direct", [] {
        Fixture fixture;
        MediaPreferences preferences;
        preferences.enableMicrophone = false;
        preferences.enableVideo = true;
        fixture.coordinator->connectDirectlyAsync("wss://direct", "direct-token", "direct-id",
                                                  "Direct User", preferences);
        TEST_CHECK(fixture.starts == 1);
        TEST_CHECK(fixture.startedUrls.back() == "wss://direct");
        TEST_CHECK(fixture.coordinator->currentMeetingId() == "direct-id");
    });
}

void VerifyErrors() {
    RunCase("Join error and retry", [] {
        Fixture fixture;
        const auto index = PreparePending(fixture, PendingStage::Join, "join-error");
        fixture.backend.completeJoin(index, false, "join-denied");
        TEST_CHECK(fixture.coordinator->state() == MeetingState::Failed);
        TEST_CHECK(fixture.errors.size() == 1);
        TEST_CHECK(fixture.errors[0].title == QString::fromUtf8("入会鉴权失败"));
        TEST_CHECK(fixture.errors[0].message == "join-denied");
        PreparePending(fixture, PendingStage::Join, "join-retry");
        TEST_CHECK(fixture.backend.joins.size() == 2);
    });
    RunCase("Token error", [] {
        Fixture fixture;
        const auto index = PreparePending(fixture, PendingStage::Token, "token-error");
        fixture.backend.completeToken(index, false, "token-error", {}, {}, "token-denied");
        TEST_CHECK(fixture.starts == 0);
        TEST_CHECK(fixture.errors.size() == 1);
        TEST_CHECK(fixture.errors[0].title == QString::fromUtf8("获取凭据失败"));
        TEST_CHECK(fixture.errors[0].message == "token-denied");
    });
    RunCase("Token missing URL", [] {
        Fixture fixture;
        const auto index = PreparePending(fixture, PendingStage::Token, "token-no-url");
        fixture.backend.completeToken(index, true, "token-no-url", {}, "token");
        TEST_CHECK(fixture.starts == 0);
        TEST_CHECK(fixture.errors.size() == 1);
        TEST_CHECK(fixture.errors[0].message == QString::fromUtf8("无法换取 LiveKit 房间访问凭证"));
    });
    RunCase("Token missing token", [] {
        Fixture fixture;
        const auto index = PreparePending(fixture, PendingStage::Token, "token-empty");
        fixture.backend.completeToken(index, true, "token-empty", "wss://token-empty", {});
        TEST_CHECK(fixture.starts == 0);
        TEST_CHECK(fixture.errors.size() == 1);
    });
    RunCase("Create error", [] {
        Fixture fixture;
        const auto index = PreparePending(fixture, PendingStage::Create, "create-error");
        fixture.backend.completeCreate(index, false, {}, {}, {}, "create-denied");
        TEST_CHECK(fixture.starts == 0);
        TEST_CHECK(fixture.errors.size() == 1);
        TEST_CHECK(fixture.errors[0].title == QString::fromUtf8("创建即时会议失败"));
        TEST_CHECK(fixture.errors[0].message == "create-denied");
    });
    RunCase("Create missing URL", [] {
        Fixture fixture;
        const auto index = PreparePending(fixture, PendingStage::Create, "create-no-url");
        fixture.backend.completeCreate(index, true, "created", {}, "token");
        TEST_CHECK(fixture.starts == 0);
        TEST_CHECK(fixture.details.empty());
        TEST_CHECK(fixture.errors.size() == 1);
    });
    RunCase("Create missing token", [] {
        Fixture fixture;
        const auto index = PreparePending(fixture, PendingStage::Create, "create-no-token");
        fixture.backend.completeCreate(index, true, "created", "wss://created", {});
        TEST_CHECK(fixture.starts == 0);
        TEST_CHECK(fixture.details.empty());
        TEST_CHECK(fixture.errors.size() == 1);
    });
}

void VerifyLeaveAndDestroy() {
    for (const auto stage : {PendingStage::Join, PendingStage::Token, PendingStage::Create}) {
        for (const bool success : {true, false}) {
            RunCase(StageName(stage) + " pending -> Leave -> late " +
                        (success ? "success" : "error"), [stage, success] {
                Fixture fixture;
                const auto index = PreparePending(fixture, stage, "leave-old");
                fixture.coordinator->leaveMeetingAsync(false);
                TEST_CHECK(fixture.coordinator->state() == MeetingState::Idle);
                const auto tokenCount = fixture.backend.tokens.size();
                const auto detailCount = fixture.details.size();
                const auto errorCount = fixture.errors.size();
                const int starts = fixture.starts;
                DeliverPending(fixture, stage, index, success, "leave-old");
                TEST_CHECK(fixture.backend.tokens.size() == tokenCount);
                TEST_CHECK(fixture.details.size() == detailCount);
                TEST_CHECK(fixture.errors.size() == errorCount);
                TEST_CHECK(fixture.starts == starts);
                TEST_CHECK(fixture.coordinator->state() == MeetingState::Idle);
            });
        }
    }
    for (const auto stage : {PendingStage::Join, PendingStage::Token, PendingStage::Create}) {
        for (const bool success : {true, false}) {
            RunCase(StageName(stage) + " queued after QObject destruction " +
                        (success ? "success" : "error"), [stage, success] {
                Fixture fixture;
                const auto index = PreparePending(fixture, stage, "destroy-old");
                const auto tokenCount = fixture.backend.tokens.size();
                const auto detailCount = fixture.details.size();
                const auto errorCount = fixture.errors.size();
                const int starts = fixture.starts;
                const int deliveries = fixture.backend.deliveries;
                bool destroyed = false;
                QPointer<MeetingCoordinator> owner(fixture.coordinator.get());
                QObject deliveryContext;
                QObject::connect(fixture.coordinator.get(), &QObject::destroyed,
                                 &deliveryContext, [&destroyed]() { destroyed = true; });
                QTimer::singleShot(0, &deliveryContext, [&fixture, stage, index, success]() {
                    DeliverPending(fixture, stage, index, success, "destroy-old");
                });
                fixture.coordinator.reset();
                TEST_CHECK(destroyed);
                TEST_CHECK(owner.isNull());
                DrainEvents();
                TEST_CHECK(fixture.backend.deliveries == deliveries + 1);
                TEST_CHECK(fixture.backend.tokens.size() == tokenCount);
                TEST_CHECK(fixture.details.size() == detailCount);
                TEST_CHECK(fixture.errors.size() == errorCount);
                TEST_CHECK(fixture.starts == starts);
            });
        }
    }
}

void VerifyReplacementOrdering() {
    for (const auto stage : {PendingStage::Join, PendingStage::Token, PendingStage::Create}) {
        for (const bool oldFirst : {true, false}) {
            RunCase(StageName(stage) + " A -> Leave -> B, " +
                        (oldFirst ? "A first" : "B first"), [stage, oldFirst] {
                Fixture fixture;
                const auto oldIndex = PreparePending(fixture, stage, "meeting-a");
                fixture.coordinator->leaveMeetingAsync(false);
                const auto newIndex = PreparePending(fixture, stage, "meeting-b");
                if (oldFirst) {
                    DeliverPending(fixture, stage, oldIndex, true, "meeting-a");
                    TEST_CHECK(fixture.starts == 0);
                    CompleteCurrent(fixture, stage, newIndex, "meeting-b");
                } else {
                    CompleteCurrent(fixture, stage, newIndex, "meeting-b");
                    DeliverPending(fixture, stage, oldIndex, false, "meeting-a");
                }
                TEST_CHECK(fixture.starts == 1);
                TEST_CHECK(fixture.errors.empty());
                TEST_CHECK(fixture.coordinator->currentMeetingId() == "meeting-b");
                if (stage == PendingStage::Create) TEST_CHECK(fixture.details.size() == 1);
            });
        }
    }
    for (const auto stage : {PendingStage::Join, PendingStage::Token, PendingStage::Create}) {
        for (const bool success : {true, false}) {
            RunCase(StageName(stage) + " A -> Direct B -> late " +
                        (success ? "success" : "error"), [stage, success] {
                Fixture fixture;
                const auto oldIndex = PreparePending(fixture, stage, "direct-old");
                fixture.coordinator->connectDirectlyAsync("wss://direct-b", "direct-b-token",
                                                          "direct-b", "Direct B", {});
                TEST_CHECK(fixture.starts == 1);
                const auto tokenCount = fixture.backend.tokens.size();
                const auto detailCount = fixture.details.size();
                DeliverPending(fixture, stage, oldIndex, success, "direct-old");
                TEST_CHECK(fixture.starts == 1);
                TEST_CHECK(fixture.backend.tokens.size() == tokenCount);
                TEST_CHECK(fixture.details.size() == detailCount);
                TEST_CHECK(fixture.errors.empty());
                TEST_CHECK(fixture.coordinator->currentMeetingId() == "direct-b");
            });
        }
    }
}

void VerifyInvalidationAndDuplicateCompletion() {
    for (const auto stage : {PendingStage::Join, PendingStage::Token, PendingStage::Create}) {
        for (const bool success : {true, false}) {
            RunCase(StageName(stage) + " pending -> invalidation -> late " +
                        (success ? "success" : "error"), [stage, success] {
                Fixture fixture;
                const auto oldIndex = PreparePending(fixture, stage, "invalid-old");
                fixture.session->invalidateSession(SessionInvalidationReason::TokenInvalid);
                DrainEvents();
                TEST_CHECK(fixture.coordinator->state() == MeetingState::Idle);
                const auto tokenCount = fixture.backend.tokens.size();
                const auto detailCount = fixture.details.size();
                const auto errorCount = fixture.errors.size();
                DeliverPending(fixture, stage, oldIndex, success, "invalid-old");
                TEST_CHECK(fixture.starts == 0);
                TEST_CHECK(fixture.backend.tokens.size() == tokenCount);
                TEST_CHECK(fixture.details.size() == detailCount);
                TEST_CHECK(fixture.errors.size() == errorCount);
                const auto joinCount = fixture.backend.joins.size();
                fixture.coordinator->joinMeetingAsync("future", {}, {}, {});
                TEST_CHECK(fixture.backend.joins.size() == joinCount);
                TEST_CHECK(fixture.backend.leaves.empty());
                TEST_CHECK(fixture.backend.ends.empty());
            });
        }
    }
    RunCase("duplicate success/error completion consumed once", [] {
        Fixture fixture;
        const auto joinIndex = PreparePending(fixture, PendingStage::Join, "duplicate");
        fixture.backend.completeJoin(joinIndex, true);
        TEST_CHECK(fixture.backend.tokens.size() == 1);
        fixture.backend.completeJoin(joinIndex, true);
        fixture.backend.completeJoin(joinIndex, false, "late-error");
        TEST_CHECK(fixture.backend.tokens.size() == 1);
        fixture.backend.completeToken(0, true, "duplicate");
        TEST_CHECK(fixture.starts == 1);
        fixture.backend.completeToken(0, true, "duplicate");
        fixture.backend.completeToken(0, false, "duplicate", {}, {}, "late-error");
        TEST_CHECK(fixture.starts == 1);
        TEST_CHECK(fixture.errors.empty());
    });
    RunCase("Join error then success consumed once", [] {
        Fixture fixture;
        const auto index = PreparePending(fixture, PendingStage::Join, "join-error-first");
        fixture.backend.completeJoin(index, false, "first-error");
        fixture.backend.completeJoin(index, true);
        TEST_CHECK(fixture.errors.size() == 1);
        TEST_CHECK(fixture.backend.tokens.empty());
    });
    RunCase("Token error then success consumed once", [] {
        Fixture fixture;
        const auto index = PreparePending(fixture, PendingStage::Token, "token-error-first");
        fixture.backend.completeToken(index, false, {}, {}, {}, "first-error");
        fixture.backend.completeToken(index, true, "token-error-first");
        TEST_CHECK(fixture.errors.size() == 1);
        TEST_CHECK(fixture.starts == 0);
    });
    RunCase("Create error then success consumed once", [] {
        Fixture fixture;
        const auto index = PreparePending(fixture, PendingStage::Create, "create-error-first");
        fixture.backend.completeCreate(index, false, {}, {}, {}, "first-error");
        fixture.backend.completeCreate(index, true, "create-error-first");
        TEST_CHECK(fixture.errors.size() == 1);
        TEST_CHECK(fixture.details.empty());
        TEST_CHECK(fixture.starts == 0);
    });
    RunCase("synchronous Join -> Token completion", [] {
        Fixture fixture;
        fixture.backend.synchronousJoin = [](ResultCallback<bool> callback) { callback(true, true, {}); };
        fixture.backend.synchronousToken = [](ResultCallback<LiveKitAuthInfo> callback) {
            callback(true, LiveKitAuthInfo{"wss://sync-join", "sync-token", "sync-join"}, {});
        };
        fixture.coordinator->joinMeetingAsync("sync-join", {}, {}, {});
        TEST_CHECK(fixture.starts == 1);
        TEST_CHECK(fixture.startedUrls.back() == "wss://sync-join");
    });
    RunCase("synchronous Quick completion", [] {
        Fixture fixture;
        fixture.backend.synchronousCreate = [](ResultCallback<LiveKitAuthInfo> callback) {
            callback(true, LiveKitAuthInfo{"wss://sync-quick", "sync-token", "sync-quick"}, {});
        };
        fixture.coordinator->createAndJoinQuickMeetingAsync("Sync Quick", 60, {});
        TEST_CHECK(fixture.starts == 1);
        TEST_CHECK(fixture.details.size() == 1);
    });
}

void VerifySessionInvalidationCleanupReentrancy() {
    RunCase("session invalidation Leaving reentrant Leave still cleans room", [] {
        Fixture fixture;
        PreparePending(fixture, PendingStage::Join, "invalidate-leave");
        MeetingCoordinatorTestAccess::markRoomSessionRunningForCleanup(*fixture.coordinator);
        TEST_CHECK(MeetingCoordinatorTestAccess::hasRoomArtifacts(*fixture.coordinator));
        bool reentered = false;
        QObject::connect(fixture.coordinator.get(), &MeetingCoordinator::stateChanged,
                         fixture.coordinator.get(), [&fixture, &reentered](MeetingState state, const QString &) {
            if (state == MeetingState::Leaving && !reentered) {
                reentered = true;
                fixture.coordinator->leaveMeetingAsync(false);
            }
        });

        fixture.session->invalidateSession(SessionInvalidationReason::TokenInvalid);
        DrainEvents();

        TEST_CHECK(reentered);
        TEST_CHECK(!MeetingCoordinatorTestAccess::hasRoomArtifacts(*fixture.coordinator));
        TEST_CHECK(fixture.coordinator->state() == MeetingState::Idle);
    });

    RunCase("session invalidation Leaving reentrant Direct rejects late response", [] {
        Fixture fixture;
        const auto oldIndex = PreparePending(fixture, PendingStage::Join, "invalidate-direct-old");
        MeetingCoordinatorTestAccess::markRoomSessionRunningForCleanup(*fixture.coordinator);
        bool reentered = false;
        QObject::connect(fixture.coordinator.get(), &MeetingCoordinator::stateChanged,
                         fixture.coordinator.get(), [&fixture, &reentered](MeetingState state, const QString &) {
            if (state == MeetingState::Leaving && !reentered) {
                reentered = true;
                fixture.coordinator->connectDirectlyAsync(
                    "wss://rejected-after-invalidation", "rejected-token",
                    "rejected-meeting", "Rejected", {});
            }
        });

        fixture.session->invalidateSession(SessionInvalidationReason::TokenInvalid);
        DrainEvents();
        TEST_CHECK(reentered);
        TEST_CHECK(!MeetingCoordinatorTestAccess::hasRoomArtifacts(*fixture.coordinator));
        TEST_CHECK(fixture.coordinator->state() == MeetingState::Idle);
        const auto tokenCount = fixture.backend.tokens.size();

        fixture.backend.completeJoin(oldIndex, true);

        TEST_CHECK(fixture.backend.tokens.size() == tokenCount);
        TEST_CHECK(fixture.starts == 0);
        TEST_CHECK(fixture.coordinator->state() == MeetingState::Idle);
    });

    RunCase("session invalidation Leaving reaches terminal Idle", [] {
        Fixture fixture;
        PreparePending(fixture, PendingStage::Create, "invalidate-terminal");
        MeetingCoordinatorTestAccess::markRoomSessionRunningForCleanup(*fixture.coordinator);

        fixture.session->invalidateSession(SessionInvalidationReason::TokenInvalid);
        DrainEvents();

        TEST_CHECK(fixture.states.size() >= 3);
        TEST_CHECK(fixture.states[fixture.states.size() - 2] == MeetingState::Leaving);
        TEST_CHECK(fixture.states.back() == MeetingState::Idle);
        TEST_CHECK(!MeetingCoordinatorTestAccess::hasRoomArtifacts(*fixture.coordinator));
    });
}

enum class ReentrantAction { Leave, Direct, Delete };

QString ActionName(ReentrantAction action) {
    switch (action) {
        case ReentrantAction::Leave: return QStringLiteral("Leave");
        case ReentrantAction::Direct: return QStringLiteral("Direct");
        case ReentrantAction::Delete: return QStringLiteral("Delete");
    }
    return QStringLiteral("Unknown");
}

void ApplyReentrantAction(Fixture &fixture, ReentrantAction action) {
    if (action == ReentrantAction::Leave) {
        fixture.coordinator->leaveMeetingAsync(false);
    } else if (action == ReentrantAction::Direct) {
        fixture.coordinator->connectDirectlyAsync("wss://reentrant-b", "reentrant-token",
                                                  "reentrant-b", "Reentrant B", {});
    } else {
        fixture.coordinator.reset();
    }
}

void VerifyReentrancy() {
    RunCase("participantsUpdated rejects nested Join", [] {
        Fixture fixture;
        bool once = false;
        QObject::connect(fixture.coordinator.get(), &MeetingCoordinator::participantsUpdated,
                         fixture.coordinator.get(), [&fixture, &once](const auto &) {
            if (once) return;
            once = true;
            fixture.coordinator->joinMeetingAsync("nested", {}, {}, {});
        });
        fixture.coordinator->joinMeetingAsync("outer", {}, {}, {});
        TEST_CHECK(fixture.backend.joins.size() == 1);
        TEST_CHECK(fixture.backend.joins[0].meetingId == "outer");
        TEST_CHECK(fixture.errors.size() == 1);
    });
    RunCase("participantsUpdated rejects nested Quick", [] {
        Fixture fixture;
        bool once = false;
        QObject::connect(fixture.coordinator.get(), &MeetingCoordinator::participantsUpdated,
                         fixture.coordinator.get(), [&fixture, &once](const auto &) {
            if (once) return;
            once = true;
            fixture.coordinator->createAndJoinQuickMeetingAsync("nested", 60, {});
        });
        fixture.coordinator->createAndJoinQuickMeetingAsync("outer", 60, {});
        TEST_CHECK(fixture.backend.creates.size() == 1);
        TEST_CHECK(fixture.backend.creates[0].value == "outer");
        TEST_CHECK(fixture.errors.size() == 1);
    });
    for (const auto action : {ReentrantAction::Leave, ReentrantAction::Direct, ReentrantAction::Delete}) {
        RunCase("participantsUpdated reentrant " + ActionName(action), [action] {
            Fixture fixture;
            bool once = false;
            QObject::connect(fixture.coordinator.get(), &MeetingCoordinator::participantsUpdated,
                             fixture.coordinator.get(), [&fixture, &once, action](const auto &) {
                if (once) return;
                once = true;
                ApplyReentrantAction(fixture, action);
            });
            fixture.coordinator->joinMeetingAsync("outer", {}, {}, {});
            TEST_CHECK(fixture.backend.joins.empty());
            TEST_CHECK(fixture.starts == (action == ReentrantAction::Direct ? 1 : 0));
            if (action == ReentrantAction::Direct) {
                TEST_CHECK(fixture.coordinator->currentMeetingId() == "reentrant-b");
            }
        });
    }
    for (const auto action : {ReentrantAction::Leave, ReentrantAction::Direct, ReentrantAction::Delete}) {
        RunCase("Validating state reentrant " + ActionName(action), [action] {
            Fixture fixture;
            QObject::connect(fixture.coordinator.get(), &MeetingCoordinator::stateChanged,
                             fixture.coordinator.get(), [&fixture, action](MeetingState state, const QString &) {
                if (state == MeetingState::Validating) ApplyReentrantAction(fixture, action);
            });
            fixture.coordinator->joinMeetingAsync("validating-a", {}, {}, {});
            TEST_CHECK(fixture.backend.joins.empty());
            TEST_CHECK(fixture.starts == (action == ReentrantAction::Direct ? 1 : 0));
        });
    }
    for (const auto action : {ReentrantAction::Leave, ReentrantAction::Direct, ReentrantAction::Delete}) {
        RunCase("FetchingCredentials state reentrant " + ActionName(action), [action] {
            Fixture fixture;
            const auto index = PreparePending(fixture, PendingStage::Join, "fetch-a");
            QObject::connect(fixture.coordinator.get(), &MeetingCoordinator::stateChanged,
                             fixture.coordinator.get(), [&fixture, action](MeetingState state, const QString &) {
                if (state == MeetingState::FetchingCredentials) ApplyReentrantAction(fixture, action);
            });
            fixture.backend.completeJoin(index, true);
            TEST_CHECK(fixture.backend.tokens.empty());
            TEST_CHECK(fixture.starts == (action == ReentrantAction::Direct ? 1 : 0));
        });
    }
    RunCase("Failed state starts B and suppresses A error", [] {
        Fixture fixture;
        const auto index = PreparePending(fixture, PendingStage::Join, "failed-a");
        QObject::connect(fixture.coordinator.get(), &MeetingCoordinator::stateChanged,
                         fixture.coordinator.get(), [&fixture](MeetingState state, const QString &) {
            if (state == MeetingState::Failed) fixture.coordinator->joinMeetingAsync("failed-b", {}, {}, {});
        });
        fixture.backend.completeJoin(index, false, "a-error");
        TEST_CHECK(fixture.backend.joins.size() == 2);
        TEST_CHECK(fixture.backend.joins[1].meetingId == "failed-b");
        TEST_CHECK(fixture.errors.empty());
        TEST_CHECK(fixture.coordinator->state() == MeetingState::Validating);
    });
    for (const auto action : {ReentrantAction::Leave, ReentrantAction::Direct, ReentrantAction::Delete}) {
        RunCase("ConnectingRoom state reentrant " + ActionName(action), [action] {
            Fixture fixture;
            const auto tokenIndex = PreparePending(fixture, PendingStage::Token, "connect-a");
            QObject::connect(fixture.coordinator.get(), &MeetingCoordinator::stateChanged,
                             fixture.coordinator.get(), [&fixture, action](MeetingState state, const QString &) {
                if (state == MeetingState::ConnectingRoom) ApplyReentrantAction(fixture, action);
            });
            fixture.backend.completeToken(tokenIndex, true, "connect-a");
            TEST_CHECK(fixture.starts == (action == ReentrantAction::Direct ? 1 : 0));
            if (fixture.coordinator) TEST_CHECK(!MeetingCoordinatorTestAccess::hasRoomArtifacts(*fixture.coordinator));
        });
    }
    for (const auto action : {ReentrantAction::Leave, ReentrantAction::Direct, ReentrantAction::Delete}) {
        RunCase("Quick detailUpdated reentrant " + ActionName(action), [action] {
            Fixture fixture;
            const auto createIndex = PreparePending(fixture, PendingStage::Create, "detail-a");
            QObject::connect(fixture.coordinator.get(), &MeetingCoordinator::meetingDetailUpdated,
                             fixture.coordinator.get(), [&fixture, action](const MeetingDetail &) {
                ApplyReentrantAction(fixture, action);
            });
            fixture.backend.completeCreate(createIndex, true, "detail-a");
            TEST_CHECK(fixture.starts == (action == ReentrantAction::Direct ? 1 : 0));
        });
    }
    RunCase("Leave Leaving signal reentrant Direct", [] {
        Fixture fixture;
        PreparePending(fixture, PendingStage::Join, "leave-a");
        QObject::connect(fixture.coordinator.get(), &MeetingCoordinator::stateChanged,
                         fixture.coordinator.get(), [&fixture](MeetingState state, const QString &) {
            if (state == MeetingState::Leaving) {
                fixture.coordinator->connectDirectlyAsync("wss://leave-b", "leave-b-token",
                                                          "leave-b", "Leave B", {});
            }
        });
        fixture.coordinator->leaveMeetingAsync(false);
        TEST_CHECK(fixture.starts == 1);
        TEST_CHECK(fixture.meetingLeftCount == 0);
        TEST_CHECK(fixture.coordinator->currentMeetingId() == "leave-b");
    });
    RunCase("Leave Idle signal reentrant Direct", [] {
        Fixture fixture;
        PreparePending(fixture, PendingStage::Join, "idle-a");
        QObject::connect(fixture.coordinator.get(), &MeetingCoordinator::stateChanged,
                         fixture.coordinator.get(), [&fixture](MeetingState state, const QString &) {
            if (state == MeetingState::Idle) {
                fixture.coordinator->connectDirectlyAsync("wss://idle-b", "idle-b-token",
                                                          "idle-b", "Idle B", {});
            }
        });
        fixture.coordinator->leaveMeetingAsync(false);
        TEST_CHECK(fixture.starts == 1);
        TEST_CHECK(fixture.meetingLeftCount == 0);
        TEST_CHECK(fixture.coordinator->currentMeetingId() == "idle-b");
    });
    RunCase("Leave Leaving signal deletes owner", [] {
        Fixture fixture;
        PreparePending(fixture, PendingStage::Join, "leave-delete");
        QObject::connect(fixture.coordinator.get(), &MeetingCoordinator::stateChanged,
                         fixture.coordinator.get(), [&fixture](MeetingState state, const QString &) {
            if (state == MeetingState::Leaving) fixture.coordinator.reset();
        });
        fixture.coordinator->leaveMeetingAsync(false);
        TEST_CHECK(!fixture.coordinator);
        TEST_CHECK(fixture.meetingLeftCount == 0);
    });
    RunCase("duplicate Leave preserves one completion", [] {
        Fixture fixture;
        PreparePending(fixture, PendingStage::Join, "leave-repeat");
        bool repeated = false;
        QObject::connect(fixture.coordinator.get(), &MeetingCoordinator::stateChanged,
                         fixture.coordinator.get(), [&fixture, &repeated](MeetingState state, const QString &) {
            if (state == MeetingState::Leaving && !repeated) {
                repeated = true;
                fixture.coordinator->leaveMeetingAsync(false);
            }
        });
        fixture.coordinator->leaveMeetingAsync(false);
        TEST_CHECK(fixture.coordinator->state() == MeetingState::Idle);
        TEST_CHECK(fixture.meetingLeftCount == 1);
        TEST_CHECK(fixture.backend.leaves.size() == 1);
    });
}

void VerifyKickBoundaries() {
    RunCase("duplicate identity kick invalidates pending admission", [] {
        Fixture fixture;
        const auto index = PreparePending(fixture, PendingStage::Join, "kick-old");
        MeetingCoordinatorTestAccess::duplicateIdentityKick(*fixture.coordinator, "duplicate");
        TEST_CHECK(fixture.coordinator->state() == MeetingState::Idle);
        TEST_CHECK(fixture.kickCount == 1);
        fixture.backend.completeJoin(index, true);
        TEST_CHECK(fixture.backend.tokens.empty());
        TEST_CHECK(fixture.starts == 0);
    });
    RunCase("duplicate identity kick Idle early return allows future admission", [] {
        Fixture fixture;
        MeetingCoordinatorTestAccess::duplicateIdentityKick(*fixture.coordinator, "idle");
        TEST_CHECK(fixture.kickCount == 0);
        PreparePending(fixture, PendingStage::Join, "after-idle-kick");
        TEST_CHECK(fixture.backend.joins.size() == 1);
    });
}

class LoopbackAdmissionServer final : public QObject {
public:
    struct Request { QString path; QJsonObject body; };

    LoopbackAdmissionServer() {
        TEST_CHECK(server.listen(QHostAddress::LocalHost, 0));
        QObject::connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (auto *socket = server.nextPendingConnection()) {
                buffers.emplace_back(socket, QByteArray{});
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket]() { receive(socket); });
                receive(socket);
            }
        });
    }

    QString baseUrl() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }
    std::vector<Request> requests;

private:
    QByteArray &bufferFor(QTcpSocket *socket) {
        for (auto &[candidate, buffer] : buffers) if (candidate == socket) return buffer;
        TEST_CHECK(false);
        return buffers.front().second;
    }

    void receive(QTcpSocket *socket) {
        auto &buffer = bufferFor(socket);
        buffer += socket->readAll();
        TEST_CHECK(buffer.size() < 65536);
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        const auto headers = buffer.left(headerEnd).split('\n');
        TEST_CHECK(!headers.empty());
        const auto requestLine = headers.front().trimmed().split(' ');
        TEST_CHECK(requestLine.size() >= 2);
        TEST_CHECK(requestLine[0] == "POST");
        int contentLength = -1;
        bool sawOperationId = false;
        bool sawToken = false;
        for (const auto &rawHeader : headers) {
            const int colon = rawHeader.indexOf(':');
            if (colon < 0) continue;
            const auto name = rawHeader.left(colon).trimmed().toLower();
            const auto value = rawHeader.mid(colon + 1).trimmed();
            if (name == "content-length") contentLength = value.toInt();
            if (name == "operationid") sawOperationId = !value.isEmpty();
            if (name == "token") sawToken = !value.isEmpty();
        }
        TEST_CHECK(contentLength >= 0);
        if (buffer.size() < headerEnd + 4 + contentLength) return;
        TEST_CHECK(sawOperationId);
        TEST_CHECK(sawToken);
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(buffer.mid(headerEnd + 4, contentLength), &error);
        TEST_CHECK(error.error == QJsonParseError::NoError);
        TEST_CHECK(document.isObject());
        const QString path = QString::fromUtf8(requestLine[1]);
        requests.push_back({path, document.object()});
        QJsonObject root;
        root["errCode"] = 0;
        root["errMsg"] = "";
        if (path == "/meeting/join_meeting") {
            root["data"] = true;
        } else if (path == "/meeting/get_meeting_token") {
            QJsonObject liveKit{{"url", "wss://loopback-join"}, {"token", "loopback-join-token"}};
            QJsonObject data{{"meetingID", "loopback-join-id"}, {"liveKit", liveKit}};
            root["data"] = data;
        } else if (path == "/meeting/create_immediate_meeting") {
            QJsonObject liveKit{{"url", "wss://loopback-quick"}, {"token", "loopback-quick-token"}};
            QJsonObject systemGenerated{{"meetingID", "loopback-quick-id"}};
            QJsonObject info{{"systemGenerated", systemGenerated}};
            QJsonObject detail{{"info", info}};
            QJsonObject data{{"liveKit", liveKit}, {"detail", detail}};
            root["data"] = data;
        } else {
            TEST_CHECK(false);
        }
        const QByteArray body = QJsonDocument(root).toJson(QJsonDocument::Compact);
        const QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
            QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        TEST_CHECK(socket->write(response) == response.size());
        socket->disconnectFromHost();
    }

    QTcpServer server;
    std::vector<std::pair<QTcpSocket *, QByteArray>> buffers;
};

void VerifyDefaultBackendLoopback() {
    RunCase("default backend loopback Join -> Token", [] {
        LoopbackAdmissionServer server;
        QTemporaryDir settingsDirectory;
        TEST_CHECK(settingsDirectory.isValid());
        auto session = SessionManagerTestAccess::create(MakeSettings(settingsDirectory.path(), server.baseUrl()));
        session->loginAsGuest("Loopback User", "loopback-user");
        auto coordinator = MeetingCoordinatorTestAccess::createDefault(*session);
        int starts = 0;
        QString startUrl;
        MeetingCoordinatorTestAccess::setRoomStartHook(*coordinator,
            [&](const QString &url, const QString &) { ++starts; startUrl = url; });
        coordinator->joinMeetingAsync("loopback-join-id", "loopback-password", "Loopback", {});
        WaitUntil([&]() { return starts == 1; });
        TEST_CHECK(server.requests.size() == 2);
        TEST_CHECK(server.requests[0].path == "/meeting/join_meeting");
        TEST_CHECK(server.requests[0].body.value("meetingID").toString() == "loopback-join-id");
        TEST_CHECK(server.requests[0].body.value("password").toString() == "loopback-password");
        TEST_CHECK(server.requests[0].body.value("userID").toString() == "loopback-user");
        TEST_CHECK(server.requests[1].path == "/meeting/get_meeting_token");
        TEST_CHECK(startUrl == "wss://loopback-join");
        TEST_CHECK(!MeetingCoordinatorTestAccess::hasRoomArtifacts(*coordinator));
        coordinator.reset();
        DrainEvents();
        session->logout(false);
    });
    RunCase("default backend loopback Quick", [] {
        LoopbackAdmissionServer server;
        QTemporaryDir settingsDirectory;
        TEST_CHECK(settingsDirectory.isValid());
        auto session = SessionManagerTestAccess::create(MakeSettings(settingsDirectory.path(), server.baseUrl()));
        session->loginAsGuest("Loopback User", "loopback-user");
        auto coordinator = MeetingCoordinatorTestAccess::createDefault(*session);
        int starts = 0;
        MeetingDetail detail;
        QObject::connect(coordinator.get(), &MeetingCoordinator::meetingDetailUpdated,
                         coordinator.get(), [&](const MeetingDetail &value) { detail = value; });
        MeetingCoordinatorTestAccess::setRoomStartHook(*coordinator,
            [&](const QString &url, const QString &token) {
                TEST_CHECK(url == "wss://loopback-quick");
                TEST_CHECK(token == "loopback-quick-token");
                ++starts;
            });
        coordinator->createAndJoinQuickMeetingAsync("Loopback Quick", 321, {});
        WaitUntil([&]() { return starts == 1; });
        TEST_CHECK(server.requests.size() == 1);
        TEST_CHECK(server.requests[0].path == "/meeting/create_immediate_meeting");
        TEST_CHECK(server.requests[0].body.value("creatorUserID").toString() == "loopback-user");
        const auto defined = server.requests[0].body.value("creatorDefinedMeetingInfo").toObject();
        TEST_CHECK(defined.value("title").toString() == "Loopback Quick");
        TEST_CHECK(defined.value("meetingDuration").toInt() == 321);
        TEST_CHECK(detail.meetingId == "loopback-quick-id");
        coordinator.reset();
        DrainEvents();
        session->logout(false);
    });
}

void RunFullMatrix() {
    VerifyNormalFlows();
    VerifyErrors();
    VerifyLeaveAndDestroy();
    VerifyReplacementOrdering();
    VerifyInvalidationAndDuplicateCompletion();
    VerifySessionInvalidationCleanupReentrancy();
    VerifyReentrancy();
    VerifyKickBoundaries();
    VerifyDefaultBackendLoopback();
    TEST_CHECK(gExecutedCases == kPlannedCases);
    TEST_CHECK(gPassedCases == kPlannedCases);
}

void RunRedOnly() {
    RunCase("RED Join pending -> Leave -> late Join success", [] {
        Fixture fixture;
        const auto index = PreparePending(fixture, PendingStage::Join, "meeting-a");
        fixture.coordinator->leaveMeetingAsync(false);
        TEST_CHECK(fixture.coordinator->state() == MeetingState::Idle);
        const auto tokenCount = fixture.backend.tokens.size();
        fixture.backend.completeJoin(index, true);
        TEST_CHECK(fixture.backend.tokens.size() == tokenCount);
        TEST_CHECK(fixture.coordinator->state() == MeetingState::Idle);
    });
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    OpenMeeting::initializeServiceEndpointPolicy(
        app.arguments().contains(QStringLiteral("--debug")));
    const bool redOnly = app.arguments().contains("--red-only");
    if (redOnly) RunRedOnly(); else RunFullMatrix();
    std::cout << "CPPQT001_CASES_PLANNED=" << (redOnly ? 1 : kPlannedCases)
              << " EXECUTED=" << gExecutedCases
              << " PASSED=" << gPassedCases << " FAILED=0" << std::endl;
    return 0;
}
