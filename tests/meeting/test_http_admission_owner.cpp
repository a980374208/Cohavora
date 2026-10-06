#include "src/core/meeting_coordinator.h"
#include "src/net/service_endpoint_policy.h"
#include "src/ui/meeting_entry_guard.h"
#include "src/telemetry/diagnostic_pipeline.h"
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

namespace livekit {
class RoomStreamDeliveryTestAccess final {
public:
    static void InstallRecoverySession(Room& room, uint64_t generation) {
        std::lock_guard lock(room.room_mutex_);
        room.session_generation_.store(generation);
        room.installed_session_generation_ = generation;
        room.connection_state_ = ConnectionState::Connected;
    }
};
}

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
               coordinator._roomListener || coordinator._sessionOwner || coordinator._stopPending;
    }

    static void markRoomSessionRunningForCleanup(MeetingCoordinator &coordinator) {
        coordinator._sessionRunning.store(true);
    }

    static void duplicateIdentityKick(MeetingCoordinator &coordinator, const QString &detail) {
        coordinator.handleDuplicateIdentityKickOff(detail);
    }
    static std::shared_ptr<asio::io_context> installRecoverySession(
        MeetingCoordinator& coordinator, const std::shared_ptr<livekit::KeyProvider>& keys) {
        auto io = std::make_shared<asio::io_context>();
        coordinator._ioContext = io;
        coordinator._sessionRuntime = std::make_shared<MeetingSessionRuntime>(*io, 77, "test", io);
        coordinator._nextSessionGeneration = 77;
        coordinator._nativeRoomGeneration = 1;
        coordinator._sessionRunning.store(true);
        coordinator._state = MeetingState::InMeeting;
        coordinator._admissionEncryption.mode = livekit::MeetingEncryptionMode::Required;
        coordinator._room = livekit::Room::Create(io->get_executor(), io);
        coordinator._room->EnableE2ee({livekit::EncryptionType::GCM, keys});
        livekit::RoomStreamDeliveryTestAccess::InstallRecoverySession(*coordinator._room, 1);
        return io;
    }
    static void recoveryReconnect(MeetingCoordinator& coordinator) {
        coordinator.setState(MeetingState::Reconnecting);
    }
    static void installUnfinishedTransfer(MeetingCoordinator& coordinator) {
        coordinator._inboundTransferLedger["reentrant-transfer"] = {InboundTransferKey{}, false};
    }
    static void recoveryNativeRestart(MeetingCoordinator& coordinator) {
        livekit::RoomStreamDeliveryTestAccess::InstallRecoverySession(*coordinator._room, 2);
    }
    static void closeRecoveryQueue(MeetingCoordinator& coordinator) {
        coordinator._sessionRuntime->revokeCallbacks();
    }
    static auto effectiveCodecs(const MeetingCoordinator& coordinator) {
        return std::pair{coordinator._mediaPrefs.cameraVideoCodec, coordinator._mediaPrefs.screenShareVideoCodec};
    }
    static auto telemetryIdentity(const MeetingCoordinator& coordinator) {
        return std::pair{coordinator._admissionTelemetry.anonymousSessionId.toStdString(),
            coordinator._admissionTelemetry.sessionGeneration};
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

constexpr int kPlannedCases = 123;
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

void VerifyEncryptionRecovery() {
    RunCase("Required recovery superseded inside transfer cancellation", [] {
        Fixture f;
        livekit::KeyProviderOptions options; options.shared_key = true;
        auto keys = std::make_shared<livekit::KeyProvider>(options);
        keys->SetSharedKey({'a', 'b', 'c'});
        auto io = MeetingCoordinatorTestAccess::installRecoverySession(*f.coordinator, keys);
        MeetingCoordinatorTestAccess::installUnfinishedTransfer(*f.coordinator);
        std::vector<bool> results;
        int cancelled = 0;
        QObject::connect(f.coordinator.get(), &MeetingCoordinator::encryptionKeyRecoveryFinished,
            f.coordinator.get(), [&](bool installed) { results.push_back(installed); });
        QObject::connect(f.coordinator.get(), &MeetingCoordinator::chatMediaReceivingFailed,
            f.coordinator.get(), [&](const QString&, const QString&) {
                ++cancelled;
                f.coordinator->recoverEncryptionKey(livekit::MeetingSecretHandle::Create({'g', 'h', 'i'}));
            });
        f.coordinator->recoverEncryptionKey(livekit::MeetingSecretHandle::Create({'d', 'e', 'f'}));
        io->poll(); DrainEvents();
        TEST_CHECK(cancelled == 1 && results.empty());
        io->restart(); io->poll(); DrainEvents();
        TEST_CHECK(results == std::vector<bool>{true});
        TEST_CHECK(keys->GetSharedKey() == std::vector<uint8_t>({'g', 'h', 'i'}));
        f.coordinator.reset(); io->restart(); io->poll(); DrainEvents();
    });
    for (const std::string scenario : {"current", "coalesced", "late-reconnect", "late-recovery", "stale-native", "closed-queue"}) {
        RunCase(QString::fromStdString("Required media status " + scenario), [scenario] {
            Fixture f;
            livekit::KeyProviderOptions options; options.shared_key = true;
            auto keys = std::make_shared<livekit::KeyProvider>(options);
            keys->SetSharedKey({'a', 'b', 'c'});
            auto io = MeetingCoordinatorTestAccess::installRecoverySession(*f.coordinator, keys);
            std::vector<livekit::MediaEncryptionStatus> results;
            QObject::connect(f.coordinator.get(), &MeetingCoordinator::encryptionMediaStatusChanged,
                f.coordinator.get(), [&](const auto& status) { results.push_back(status); });
            if (scenario == "closed-queue") MeetingCoordinatorTestAccess::closeRecoveryQueue(*f.coordinator);
            if (scenario == "stale-native") MeetingCoordinatorTestAccess::recoveryNativeRestart(*f.coordinator);
            f.coordinator->refreshEncryptionMediaStatus();
            if (scenario == "coalesced") f.coordinator->refreshEncryptionMediaStatus();
            io->poll();
            if (scenario == "late-reconnect") MeetingCoordinatorTestAccess::recoveryReconnect(*f.coordinator);
            if (scenario == "late-recovery") f.coordinator->recoverEncryptionKey(livekit::MeetingSecretHandle::Create({'d', 'e', 'f'}));
            DrainEvents();
            if (scenario == "current" || scenario == "coalesced") {
                TEST_CHECK(results.size() == 1 && results[0].enabled);
                TEST_CHECK(results[0].native_generation == 1 && results[0].tracks.empty());
            } else TEST_CHECK(results.empty());
            f.coordinator.reset(); io->restart(); io->poll(); DrainEvents();
        });
    }
    for (const std::string scenario : {"success", "supersede", "cancel", "stale-native", "late-ui", "closed-queue"}) {
        RunCase(QString::fromStdString("Required recovery " + scenario), [scenario] {
            Fixture f;
            livekit::KeyProviderOptions options; options.shared_key = true;
            auto keys = std::make_shared<livekit::KeyProvider>(options);
            keys->SetSharedKey({'a', 'b', 'c'});
            auto io = MeetingCoordinatorTestAccess::installRecoverySession(*f.coordinator, keys);
            std::vector<bool> results;
            QObject::connect(f.coordinator.get(), &MeetingCoordinator::encryptionKeyRecoveryFinished,
                f.coordinator.get(), [&](bool installed) { results.push_back(installed); });
            if (scenario == "closed-queue") MeetingCoordinatorTestAccess::closeRecoveryQueue(*f.coordinator);
            auto first = livekit::MeetingSecretHandle::Create({'d', 'e', 'f'});
            f.coordinator->recoverEncryptionKey(first);
            std::shared_ptr<livekit::MeetingSecretHandle> second;
            if (scenario == "supersede") {
                second = livekit::MeetingSecretHandle::Create({'g', 'h', 'i'});
                f.coordinator->recoverEncryptionKey(second);
                TEST_CHECK(!first->available());
            }
            if (scenario == "cancel") MeetingCoordinatorTestAccess::recoveryReconnect(*f.coordinator);
            if (scenario == "stale-native") MeetingCoordinatorTestAccess::recoveryNativeRestart(*f.coordinator);
            io->poll();
            if (scenario == "late-ui") MeetingCoordinatorTestAccess::recoveryReconnect(*f.coordinator);
            DrainEvents();
            TEST_CHECK(!first->available());
            if (scenario == "success" || scenario == "supersede") {
                TEST_CHECK(results == std::vector<bool>{true});
                TEST_CHECK(keys->GetSharedKey() == (scenario == "success" ?
                    std::vector<uint8_t>{'d', 'e', 'f'} : std::vector<uint8_t>{'g', 'h', 'i'}));
                TEST_CHECK(f.coordinator->canRecoverEncryptionKey());
            } else if (scenario == "closed-queue" || scenario == "stale-native") {
                TEST_CHECK(results == std::vector<bool>{false});
                TEST_CHECK(keys->GetSharedKey() == std::vector<uint8_t>({'a', 'b', 'c'}));
            } else {
                TEST_CHECK(results.empty());
                if (scenario == "cancel") TEST_CHECK(keys->GetSharedKey() == std::vector<uint8_t>({'a', 'b', 'c'}));
            }
            f.coordinator.reset(); io->restart(); io->poll(); DrainEvents();
        });
    }
}

void VerifyEncryptionAdmission() {
    VerifyEncryptionRecovery();
    using livekit::MeetingEncryptionMode;
    using livekit::MeetingEncryptionRequest;
    using livekit::MeetingSecretHandle;
    RunCase("Required unsupported codec rejects all admission APIs before network", [] {
        for (int entry = 0; entry < 3; ++entry) for (bool camera : {false, true}) {
            Fixture f;
            OpenMeeting::MediaPreferences prefs;
            prefs.cameraVideoCodec = "h264";
            (camera ? prefs.cameraVideoCodec : prefs.screenShareVideoCodec) = camera ? "vp9" : "av1";
            auto secret = MeetingSecretHandle::Create({'a', 'b', 'c', 'd', 'e', 'f'});
            MeetingEncryptionRequest request{MeetingEncryptionMode::Required, secret};
            int visibleErrors = 0;
            QObject::connect(f.coordinator.get(), &MeetingCoordinator::errorOccurred,
                f.coordinator.get(), [&](const QString&, const QString&) {
                    // Mirrors the meeting window's startup-error admission.
                    if (f.coordinator->state() == MeetingState::Failed) ++visibleErrors;
                });
            if (entry == 0) f.coordinator->joinMeetingAsync("id", "", "name", prefs, request);
            else if (entry == 1) f.coordinator->createAndJoinQuickMeetingAsync("title", 900, prefs, request);
            else f.coordinator->connectDirectlyAsync("wss://test", "token", "id", "name", prefs, request);
            TEST_CHECK(!secret->available() && f.errors.size() == 1);
            TEST_CHECK(visibleErrors == 1 && f.coordinator->state() == MeetingState::Failed);
            TEST_CHECK(f.errors.front().message.contains("Auto") &&
                f.errors.front().message.contains("VP8") && f.errors.front().message.contains("H264"));
            TEST_CHECK(f.backend.dispatches == 0 && f.starts == 0);
            TEST_CHECK(!MeetingCoordinatorTestAccess::hasRoomArtifacts(*f.coordinator));
            TEST_CHECK(f.coordinator->currentMeetingId().isEmpty());
            TEST_CHECK((camera ? prefs.cameraVideoCodec : prefs.screenShareVideoCodec) == (camera ? "vp9" : "av1"));
        }
    });
    for (int entry = 0; entry < 3; ++entry) for (bool required : {false, true}) {
        RunCase(QString("Encryption codec admission entry %1 required %2").arg(entry).arg(required), [entry, required] {
            Fixture f;
            OpenMeeting::MediaPreferences prefs;
            prefs.cameraVideoCodec = required ? "auto" : "vp9";
            prefs.screenShareVideoCodec = required ? "h264" : "av1";
            MeetingEncryptionRequest request;
            if (required) request = {MeetingEncryptionMode::Required, MeetingSecretHandle::Create({'a', 'b', 'c'})};
            if (entry == 0) f.coordinator->joinMeetingAsync("id", "", "name", prefs, request);
            else if (entry == 1) f.coordinator->createAndJoinQuickMeetingAsync("title", 900, prefs, request);
            else f.coordinator->connectDirectlyAsync("wss://test", "token", "id", "name", prefs, request);
            TEST_CHECK(f.errors.empty());
            const auto codecs = MeetingCoordinatorTestAccess::effectiveCodecs(*f.coordinator);
            TEST_CHECK(codecs.first == (required ? "vp8" : "vp9"));
            TEST_CHECK(codecs.second == (required ? "h264" : "av1"));
            TEST_CHECK(prefs.cameraVideoCodec == (required ? "auto" : "vp9"));
        });
    }
    RunCase("Required missing key rejected at all three entry APIs", [] {
        Fixture f;
        MeetingEncryptionRequest request{MeetingEncryptionMode::Required, {}};
        f.coordinator->joinMeetingAsync("id", "", "name", {}, request);
        TEST_CHECK(f.coordinator->state() == MeetingState::Failed);
        f.coordinator->createAndJoinQuickMeetingAsync("title", 900, {}, request);
        TEST_CHECK(f.coordinator->state() == MeetingState::Failed);
        f.coordinator->connectDirectlyAsync("wss://test", "token", "id", "name", {}, request);
        TEST_CHECK(f.backend.joins.empty() && f.backend.creates.empty() && f.starts == 0);
        TEST_CHECK(f.errors.size() == 3 && f.coordinator->state() == MeetingState::Failed);
    });
    for (int action = 0; action < 4; ++action) {
        RunCase(QString("Encryption preflight Failed reentrant action %1").arg(action), [action] {
            for (int entry = 0; entry < 3; ++entry) for (bool codecFailure : {false, true}) {
                Fixture f;
                auto rejected = MeetingSecretHandle::Create({'a', 'b', 'c', 'd', 'e', 'f'});
                auto replacement = MeetingSecretHandle::Create({'g', 'h', 'i', 'j', 'k', 'l'});
                const auto authGeneration = f.session->authGeneration();
                const MeetingEncryptionRequest request{MeetingEncryptionMode::Required,
                    codecFailure ? rejected : nullptr};
                OpenMeeting::MediaPreferences prefs;
                if (codecFailure) prefs.screenShareVideoCodec = "av1";
                QObject::connect(f.coordinator.get(), &MeetingCoordinator::stateChanged,
                    f.coordinator.get(), [&](MeetingState state, const QString&) {
                        if (state != MeetingState::Failed) return;
                        if (action == 0) f.coordinator->leaveMeetingAsync();
                        else if (action == 1) {
                            f.coordinator->connectDirectlyAsync("wss://replacement", "token", "replacement", "name", {},
                                {MeetingEncryptionMode::Required, replacement});
                        } else if (action == 2) f.coordinator.reset();
                        else f.session->loginAsGuest("New User", "new-user");
                    });
                if (entry == 0) f.coordinator->joinMeetingAsync("id", "", "name", prefs, request);
                else if (entry == 1) f.coordinator->createAndJoinQuickMeetingAsync("title", 900, prefs, request);
                else f.coordinator->connectDirectlyAsync("wss://test", "token", "id", "name", prefs, request);
                TEST_CHECK(f.errors.empty() && f.backend.dispatches == 0);
                if (codecFailure) TEST_CHECK(!rejected->available());
                TEST_CHECK(f.starts == (action == 1 ? 1 : 0));
                if (action == 0) TEST_CHECK(f.coordinator->state() == MeetingState::Idle);
                else if (action == 1) {
                    TEST_CHECK(f.coordinator->state() == MeetingState::ConnectingRoom);
                    TEST_CHECK(f.coordinator->currentMeetingId() == "replacement" && replacement->available());
                } else if (action == 2) TEST_CHECK(!f.coordinator);
                else {
                    TEST_CHECK(f.session->authGeneration() != authGeneration && f.session->userId() == "new-user");
                    TEST_CHECK(f.coordinator->state() == MeetingState::Failed);
                }
            }
        });
    }
    RunCase("Required pending Join cancel revokes external aliases", [] {
        Fixture f;
        auto secret = MeetingSecretHandle::Create({'a', 'b', 'c'});
        f.coordinator->joinMeetingAsync("id", "", "name", {}, {MeetingEncryptionMode::Required, secret});
        TEST_CHECK(secret->available());
        f.coordinator->leaveMeetingAsync();
        TEST_CHECK(!secret->available());
        f.backend.completeJoin(0, true);
        TEST_CHECK(f.backend.tokens.empty() && f.starts == 0);
    });
    RunCase("Busy admission revokes only the rejected new secret", [] {
        Fixture f;
        auto active = MeetingSecretHandle::Create({'a', 'b', 'c'});
        auto rejected = MeetingSecretHandle::Create({'d', 'e', 'f'});
        f.coordinator->joinMeetingAsync("id", "", "name", {}, {MeetingEncryptionMode::Required, active});
        f.coordinator->joinMeetingAsync("other", "", "name", {}, {MeetingEncryptionMode::Required, rejected});
        TEST_CHECK(active->available() && !rejected->available());
        TEST_CHECK(f.backend.joins.size() == 1);
    });
    RunCase("Error signal may destroy Coordinator before rejected secret cleanup", [] {
        Fixture f;
        auto active = MeetingSecretHandle::Create({'a', 'b', 'c'});
        auto rejected = MeetingSecretHandle::Create({'d', 'e', 'f'});
        f.coordinator->joinMeetingAsync("id", "", "name", {}, {MeetingEncryptionMode::Required, active});
        QObject::connect(f.coordinator.get(), &MeetingCoordinator::errorOccurred,
            f.coordinator.get(), [&] { f.coordinator.reset(); });
        f.coordinator->joinMeetingAsync("other", "", "name", {}, {MeetingEncryptionMode::Required, rejected});
        TEST_CHECK(!f.coordinator && !active->available() && !rejected->available());
    });
    RunCase("Duplicate admission sharing the active handle does not cancel it", [] {
        Fixture f;
        auto secret = MeetingSecretHandle::Create({'a', 'b', 'c'});
        const MeetingEncryptionRequest request{MeetingEncryptionMode::Required, secret};
        f.coordinator->joinMeetingAsync("id", "", "name", {}, request);
        f.coordinator->joinMeetingAsync("id", "", "name", {}, request);
        TEST_CHECK(secret->available() && f.backend.joins.size() == 1);
    });
    RunCase("Required authentication failure revokes key", [] {
        Fixture f;
        auto secret = MeetingSecretHandle::Create({'a', 'b', 'c'});
        f.coordinator->joinMeetingAsync("id", "", "name", {}, {MeetingEncryptionMode::Required, secret});
        f.backend.completeJoin(0, false, "denied");
        TEST_CHECK(!secret->available() && f.starts == 0);
    });
    RunCase("Required pending Create cancel revokes key", [] {
        Fixture f;
        auto secret = MeetingSecretHandle::Create({'a', 'b', 'c'});
        f.coordinator->createAndJoinQuickMeetingAsync("title", 900, {}, {MeetingEncryptionMode::Required, secret});
        TEST_CHECK(secret->available());
        f.coordinator->leaveMeetingAsync();
        TEST_CHECK(!secret->available() && f.starts == 0);
    });
    RunCase("Required direct request is retained until session ownership boundary", [] {
        Fixture f;
        auto secret = MeetingSecretHandle::Create({'a', 'b', 'c'});
        f.coordinator->connectDirectlyAsync("wss://test", "token", "id", "name", {}, {MeetingEncryptionMode::Required, secret});
        // Fixture intercepts Room construction; native provider consumption is tested separately.
        TEST_CHECK(f.starts == 1 && secret->available());
        f.coordinator->leaveMeetingAsync();
        TEST_CHECK(!secret->available());
    });
    RunCase("Coordinator destruction revokes pending Required request", [] {
        auto secret = MeetingSecretHandle::Create({'a', 'b', 'c'});
        {
            Fixture f;
            f.coordinator->joinMeetingAsync("id", "", "name", {}, {MeetingEncryptionMode::Required, secret});
        }
        TEST_CHECK(!secret->available());
    });
}

void VerifyNormalFlows() {
    VerifyEncryptionAdmission();
    RunCase("normal Join -> Token", [] {
        Fixture fixture;
        auto pipeline = std::make_shared<livekit::diagnostic::DiagnosticPipeline>();
        livekit::diagnostic::InstallBusinessPipeline(pipeline);
        const auto index = PreparePending(fixture, PendingStage::Join, "normal-join");
        const auto [session, generation] = MeetingCoordinatorTestAccess::telemetryIdentity(*fixture.coordinator);
        TEST_CHECK(generation != 0);
        const auto admission = pipeline->RecentTimeline(session, generation);
        TEST_CHECK(!admission.events.empty());
        TEST_CHECK(admission.events.front().kind == livekit::diagnostic::EventKind::AdmissionStarted);
        TEST_CHECK(admission.events.front().context.session_generation == generation);
        CompleteCurrent(fixture, PendingStage::Join, index, "normal-join");
        TEST_CHECK(fixture.starts == 1);
        TEST_CHECK(fixture.startedUrls.back() == "wss://normal-join");
        TEST_CHECK(fixture.coordinator->state() == MeetingState::ConnectingRoom);
        TEST_CHECK(fixture.errors.empty());
        Fixture another;
        PreparePending(another, PendingStage::Join, "another-owner");
        const auto [otherSession, otherGeneration] = MeetingCoordinatorTestAccess::telemetryIdentity(*another.coordinator);
        TEST_CHECK(otherGeneration > generation && otherSession != session);
        const auto otherAdmission = pipeline->RecentTimeline(otherSession, otherGeneration);
        TEST_CHECK(!otherAdmission.events.empty());
        TEST_CHECK(otherAdmission.events.front().context.session_generation == otherGeneration);
        livekit::diagnostic::InstallBusinessPipeline({});
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
        TEST_CHECK(fixture.details.back().creatorUserId == "fixture-user");
        TEST_CHECK(fixture.coordinator->isHost());
    });
    RunCase("normal Direct", [] {
        Fixture fixture;
        MediaPreferences preferences;
        preferences.enableMicrophone = false;
        preferences.enableVideo = true;
        fixture.coordinator->connectDirectlyAsync("wss://direct", "direct-token", "direct-id",
                                                  "Direct User", preferences);
        TEST_CHECK(fixture.starts == 1);
        TEST_CHECK(fixture.backend.dispatches == 0);
        TEST_CHECK(fixture.startedUrls.back() == "wss://direct");
        TEST_CHECK(fixture.coordinator->currentMeetingId() == "direct-id");
    });
}

void VerifyEntryMediaAvailability() {
    for (const auto entry : {QStringLiteral("Join"), QStringLiteral("Quick"),
                            QStringLiteral("Direct")}) {
        RunCase(entry + " preserves unavailable media and user intent", [entry] {
            Fixture fixture;
            auto &coordinator = *fixture.coordinator;
            // A window may discover missing devices before its sole explicit
            // entry call applies the saved preferences.
            coordinator.setLocalAudioMuted(true);
            coordinator.setLocalVideoEnabled(false);
            coordinator.setLocalAudioAvailable(false);
            coordinator.setLocalVideoAvailable(false);
            const auto check = [&](bool muted, bool videoEnabled) {
                TEST_CHECK(coordinator.isLocalAudioMuted() == muted);
                TEST_CHECK(coordinator.isLocalVideoEnabled() == videoEnabled);
                bool foundLocal = false;
                for (const auto &participant : coordinator.participants()) {
                    if (!participant.isLocal) continue;
                    foundLocal = true;
                    TEST_CHECK(participant.isAudioMuted == muted);
                    TEST_CHECK(participant.isVideoEnabled == videoEnabled);
                }
                TEST_CHECK(foundLocal);
            };
            MediaPreferences preferences;
            preferences.enableMicrophone = true;
            preferences.enableVideo = true;
            if (entry == QStringLiteral("Join")) {
                coordinator.joinMeetingAsync("media-entry", {}, "Media User", preferences);
                check(true, false);
                fixture.backend.completeJoin(0, true);
                check(true, false);
                fixture.backend.completeToken(0, true, "media-entry");
            } else if (entry == QStringLiteral("Quick")) {
                coordinator.createAndJoinQuickMeetingAsync("Media Entry", 900, preferences);
                check(true, false);
                fixture.backend.completeCreate(0, true, "media-entry");
            } else {
                coordinator.connectDirectlyAsync("wss://fixture.invalid", "fixture-token",
                                                  "media-entry", "Media User", preferences);
            }
            TEST_CHECK(fixture.starts == 1);
            TEST_CHECK(!MeetingCoordinatorTestAccess::hasRoomArtifacts(coordinator));
            check(true, false);

            // Entry resets intent to preferences without overriding readiness;
            // recovery can now honor the requested enable state.
            coordinator.setLocalAudioAvailable(true);
            coordinator.setLocalVideoAvailable(true);
            check(false, true);
            coordinator.setLocalAudioAvailable(false);
            coordinator.setLocalVideoAvailable(false);
            coordinator.setLocalAudioMuted(true);
            coordinator.setLocalVideoEnabled(false);
            coordinator.setLocalAudioAvailable(true);
            coordinator.setLocalVideoAvailable(true);
            check(true, false);
            TEST_CHECK(fixture.starts == 1);
        });
    }
}

void VerifyErrors() {
    RunCase("Join error and retry", [] {
        Fixture fixture;
        const auto index = PreparePending(fixture, PendingStage::Join, "join-error");
        fixture.backend.completeJoin(index, false, "join-denied");
        TEST_CHECK(fixture.coordinator->state() == MeetingState::Failed);
        TEST_CHECK(fixture.errors.size() == 1);
        TEST_CHECK(fixture.errors[0].title == QString::fromUtf8("Meeting Authentication Failed"));
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
        TEST_CHECK(fixture.errors[0].title == QString::fromUtf8("Unable to Obtain Credentials"));
        TEST_CHECK(fixture.errors[0].message == "token-denied");
    });
    RunCase("Token missing URL", [] {
        Fixture fixture;
        const auto index = PreparePending(fixture, PendingStage::Token, "token-no-url");
        fixture.backend.completeToken(index, true, "token-no-url", {}, "token");
        TEST_CHECK(fixture.starts == 0);
        TEST_CHECK(fixture.errors.size() == 1);
        TEST_CHECK(fixture.errors[0].message == QString::fromUtf8("Unable to obtain LiveKit room credentials"));
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
        TEST_CHECK(fixture.errors[0].title == QString::fromUtf8("Unable to Create Instant Meeting"));
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
            RunCase(StageName(stage) + " callback rejected while session invalidation is queued " +
                        (success ? "success" : "error"), [stage, success] {
                Fixture fixture;
                const auto pendingIndex = PreparePending(fixture, stage, "invalidating-current");
                const auto tokenCount = fixture.backend.tokens.size();
                const auto detailCount = fixture.details.size();
                const auto errorCount = fixture.errors.size();

                fixture.session->invalidateSession(SessionInvalidationReason::TokenInvalid);
                TEST_CHECK(fixture.session->isSessionInvalidating());
                DeliverPending(fixture, stage, pendingIndex, success, "invalidating-current");

                TEST_CHECK(fixture.starts == 0);
                TEST_CHECK(fixture.backend.tokens.size() == tokenCount);
                TEST_CHECK(fixture.details.size() == detailCount);
                TEST_CHECK(fixture.errors.size() == errorCount);
                DrainEvents();
                TEST_CHECK(fixture.coordinator->state() == MeetingState::Idle);
                TEST_CHECK(!MeetingCoordinatorTestAccess::hasRoomArtifacts(*fixture.coordinator));
            });
        }
    }

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
    RunCase("Required admission keeps secret out of HTTP bodies", [] {
        for (const bool quick : {false, true}) {
            LoopbackAdmissionServer server;
            QTemporaryDir settingsDirectory;
            TEST_CHECK(settingsDirectory.isValid());
            auto session = SessionManagerTestAccess::create(MakeSettings(settingsDirectory.path(), server.baseUrl()));
            session->loginAsGuest("Loopback User", "loopback-user");
            auto coordinator = MeetingCoordinatorTestAccess::createDefault(*session);
            int starts = 0;
            MeetingCoordinatorTestAccess::setRoomStartHook(*coordinator,
                [&](const QString&, const QString&) { ++starts; });
            const QByteArray marker("public-e2ee-http-isolation-marker");
            auto secret = livekit::MeetingSecretHandle::Create(
                std::vector<uint8_t>(marker.begin(), marker.end()));
            livekit::MeetingEncryptionRequest request{livekit::MeetingEncryptionMode::Required, secret};
            if (quick) coordinator->createAndJoinQuickMeetingAsync("Loopback Quick", 321, {}, request);
            else coordinator->joinMeetingAsync("loopback-join-id", "loopback-password", "Loopback", {}, request);
            WaitUntil([&] { return starts == 1; });
            TEST_CHECK(server.requests.size() == (quick ? 1 : 2));
            for (const auto& request : server.requests) {
                const auto bytes = QJsonDocument(request.body).toJson(QJsonDocument::Compact);
                TEST_CHECK(!bytes.contains(marker) && !bytes.contains(marker.toBase64()));
                TEST_CHECK(!bytes.contains("secret") && !bytes.contains("encryptionKey"));
            }
            coordinator.reset(); DrainEvents();
            TEST_CHECK(!secret->available());
            session->logout(false);
        }
    });
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

void VerifyApplicationMeetingEntryGuard() {
    for (const auto stage : {PendingStage::Join, PendingStage::Token, PendingStage::Create}) {
        RunCase("application busy during " + StageName(stage) + " and late completion", [stage] {
            MeetingUI::MeetingEntryGuard guard;
            auto reservation = guard.tryAcquire();
            TEST_CHECK(reservation);
            // The reservation exists before even the login/join dialog opens.
            TEST_CHECK(!guard.tryAcquire());
            Fixture fixture;
            const auto index = PreparePending(fixture, stage, "busy-old");
            const auto dispatches = fixture.backend.dispatches;
            if (auto second = guard.tryAcquire()) {
                fixture.coordinator->createAndJoinQuickMeetingAsync("blocked", 900, {});
                TEST_CHECK(false);
            }
            TEST_CHECK(fixture.backend.dispatches == dispatches);

            // Closing an admission window cancels its Coordinator before
            // destroying the reservation child. A late reply cannot reopen it
            // or release a successor's reservation.
            fixture.coordinator->leaveMeetingAsync();
            TEST_CHECK(!guard.tryAcquire());
            reservation.reset();
            auto successor = guard.tryAcquire();
            TEST_CHECK(successor);
            DeliverPending(fixture, stage, index, true, "busy-old");
            TEST_CHECK(fixture.starts == 0);
            TEST_CHECK(!guard.tryAcquire());
        });
    }

    RunCase("application busy releases on dialog cancellation", [] {
        MeetingUI::MeetingEntryGuard guard;
        {
            auto dialogReservation = guard.tryAcquire();
            TEST_CHECK(dialogReservation);
            TEST_CHECK(!guard.tryAcquire());
        }
        TEST_CHECK(guard.tryAcquire());
    });

    RunCase("failed meeting retains reservation through window cleanup", [] {
        MeetingUI::MeetingEntryGuard guard;
        Fixture fixture;
        auto reservation = guard.tryAcquire();
        TEST_CHECK(reservation);
        const auto index = PreparePending(fixture, PendingStage::Create, "failed-window");
        fixture.backend.completeCreate(index, false, {}, {}, {}, "expected failure");
        TEST_CHECK(fixture.coordinator->state() == MeetingState::Failed);

        class WindowOwner final : public QObject {
        public:
            explicit WindowOwner(std::function<void()> cleanup) : _cleanup(std::move(cleanup)) {}
            ~WindowOwner() override { _cleanup(); }
        private:
            std::function<void()> _cleanup;
        };
        auto window = std::make_unique<WindowOwner>([&] {
            TEST_CHECK(!guard.tryAcquire());
            fixture.coordinator.reset();
            TEST_CHECK(!guard.tryAcquire());
        });
        reservation->setParent(window.get());
        reservation.release();
        // A failed window can still own capture/render resources while its
        // notice is visible; state alone must not free application capacity.
        TEST_CHECK(!guard.tryAcquire());
        window.reset();
        auto next = guard.tryAcquire();
        TEST_CHECK(next);
        fixture.coordinator = MeetingCoordinatorTestAccess::create(*fixture.session, fixture.backend.functions());
        fixture.installObservers();
        fixture.coordinator->connectDirectlyAsync("wss://fixture.invalid", "fixture-token", "next", "user", {});
        TEST_CHECK(fixture.starts == 1);
        TEST_CHECK(!guard.tryAcquire());
    });
}

void RunFullMatrix() {
    VerifyApplicationMeetingEntryGuard();
    VerifyNormalFlows();
    VerifyEntryMediaAvailability();
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
