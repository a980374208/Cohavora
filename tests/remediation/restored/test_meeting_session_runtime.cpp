// Phase2A1 delivered test copy; original input remains immutable and untracked.
// Original: tests/test_meeting_session_runtime.cpp
// Original SHA256: 163325d59fc2ff2de2488c48a2ba9a6caf01b0a803526fb3648605fb9fb25a4b
// Provenance and exact adaptations: tests/remediation/restored/PROVENANCE.json
// Adaptation: provenance comments only; original active checks retained.

// Added coverage: callback revocation, managed shutdown/drain, repeated Quit, and modal unwind.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QMetaObject>
#include <QtCore/QProcess>
#include <QtCore/QTimer>

#include "src/app/async_shutdown_guard.h"
#include "src/core/meeting_session_runtime.h"
#include "src/core/session_shutdown_service.h"

namespace {

void Require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::abort();
    }
}

template <typename Predicate>
void ProcessUntil(Predicate predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        std::this_thread::yield();
    }
    Require(predicate(), message);
}

void TestQtCallbackGate() {
    auto receiver = std::make_unique<QObject>();
    auto gate = OpenMeeting::QtCallbackGate<QObject>::Create(receiver.get());
    bool delivered = false;
    std::thread producer([&] {
        Require(gate->Post([&](QObject* target) {
            Require(target == receiver.get(), "gate delivered the wrong receiver");
            Require(QThread::currentThread() == QCoreApplication::instance()->thread(),
                    "gate inspected or called receiver off the Qt thread");
            delivered = true;
        }), "active gate rejected callback");
    });
    producer.join();
    Require(!delivered, "gate delivery was not queued");
    ProcessUntil([&] { return delivered; }, "gate callback was not delivered");

    int rejectedDeliveries = 0;
    Require(gate->Post([&](QObject*) { ++rejectedDeliveries; }),
            "gate rejected work before revocation");
    gate->Revoke();
    Require(!gate->active(), "gate did not revoke native admission");
    Require(!gate->Post([&](QObject*) { ++rejectedDeliveries; }),
            "revoked gate accepted more work");
    QCoreApplication::processEvents();
    Require(rejectedDeliveries == 0, "revocation failed to drop queued work");

    auto deletedReceiverGate = OpenMeeting::QtCallbackGate<QObject>::Create(receiver.get());
    Require(deletedReceiverGate->Post([&](QObject*) { ++rejectedDeliveries; }),
            "receiver gate rejected work before deletion");
    receiver.reset();
    Require(!deletedReceiverGate->active(), "deleted receiver retained active admission");
    std::thread lateProducer([&] {
        Require(!deletedReceiverGate->Post([&](QObject*) { ++rejectedDeliveries; }),
                "deleted receiver accepted a late callback");
    });
    lateProducer.join();
    QCoreApplication::processEvents();
    Require(rejectedDeliveries == 0, "queued callback reached a deleted receiver");
}

void TestShutdownService() {
    OpenMeeting::SessionShutdownService service;
    auto receiver = std::make_unique<QObject>();
    auto gate = OpenMeeting::QtCallbackGate<QObject>::Create(receiver.get());
    std::promise<void> releaseWorker;
    auto release = releaseWorker.get_future().share();
    std::atomic<bool> workerEntered{false};
    bool uiResponsive = false;
    bool drained = false;
    bool stopped = false;
    bool failureDelivered = false;
    int jobsCompleted = 0;

    Require(service.Submit([&, release] {
        Require(QThread::currentThread() != QCoreApplication::instance()->thread(),
                "shutdown job ran on the Qt thread");
        workerEntered.store(true, std::memory_order_release);
        release.wait();
        ++jobsCompleted;
        gate->Post([&](QObject*) { uiResponsive = true; });
    }), "cleanup job was rejected");
    ProcessUntil([&] { return workerEntered.load(std::memory_order_acquire); },
                 "shutdown worker did not start");
    Require(service.busy() && service.pending() == 1,
            "running cleanup job was omitted from pending count");
    Require(gate->Post([&](QObject*) { uiResponsive = true; }),
            "heartbeat callback was rejected");
    ProcessUntil([&] { return uiResponsive; },
                 "Qt could not process events while cleanup was blocked");

    Require(service.Submit([] { throw std::runtime_error("controlled cleanup failure"); },
        [&](std::exception_ptr failure) {
            Require(failure != nullptr, "cleanup exception was not delivered");
            Require(QThread::currentThread() == QCoreApplication::instance()->thread(),
                    "cleanup completion ran off the Qt thread");
            failureDelivered = true;
        }), "throwing cleanup job was rejected");
    Require(service.Submit([&] { ++jobsCompleted; }),
            "job after a controlled failure was rejected");
    service.DrainAsync([&] {
        Require(jobsCompleted == 3 && failureDelivered,
                "drain ran before accepted jobs and their queued completion");
        drained = true;
    });
    service.ShutdownAsync([&] {
        Require(drained && !service.busy(), "shutdown completed before drain");
        stopped = true;
    });
    QCoreApplication::processEvents();
    Require(!drained && !stopped, "active cleanup was reported as stopped");
    Require(!service.Submit([] {}), "shutdown accepted a new job");
    Require(service.SubmitCleanup([&] { ++jobsCompleted; }),
            "draining service lost late ownership-bearing cleanup");
    releaseWorker.set_value();
    ProcessUntil([&] { return stopped; }, "asynchronous service shutdown did not complete");
    bool repeatedStop = false;
    service.ShutdownAsync([&] { repeatedStop = true; });
    ProcessUntil([&] { return repeatedStop; }, "repeated shutdown did not complete");
}

void TestShutdownDrainReentry() {
    OpenMeeting::SessionShutdownService service;
    std::promise<void> releaseFollowup;
    auto release = releaseFollowup.get_future().share();
    std::atomic<bool> followupEntered{false};
    bool drained = false;
    bool stopped = false;
    Require(service.Submit([] {}, [&](std::exception_ptr failure) {
        Require(!failure, "successful job reported a failure");
        Require(service.Submit([&, release] {
            followupEntered.store(true, std::memory_order_release);
            release.wait();
        }), "Qt completion could not enqueue its follow-up cleanup");
    }), "initial reentry job was rejected");
    service.DrainAsync([&] { drained = true; });
    ProcessUntil([&] { return followupEntered.load(std::memory_order_acquire); },
                 "completion did not start follow-up cleanup");
    QCoreApplication::processEvents();
    Require(!drained, "stale idle notification ignored reentrant cleanup");
    releaseFollowup.set_value();
    ProcessUntil([&] { return drained; }, "drain did not await reentrant cleanup");
    service.ShutdownAsync([&] { stopped = true; });
    ProcessUntil([&] { return stopped; }, "reentry service did not shut down");
}

struct RetainedCleanupWitness {
    explicit RetainedCleanupWitness(std::shared_ptr<std::atomic<int>> destroyed)
        : destroyed(std::move(destroyed)) {}
    ~RetainedCleanupWitness() {
        destroyed->fetch_add(1, std::memory_order_release);
    }
    std::shared_ptr<std::atomic<int>> destroyed;
};

[[noreturn]] void RunRetainedCleanupFailureChild() {
    auto service = new OpenMeeting::SessionShutdownService();
    auto destroyed = std::make_shared<std::atomic<int>>(0);
    auto owner = std::make_shared<RetainedCleanupWitness>(destroyed);
    std::weak_ptr<RetainedCleanupWitness> weakOwner = owner;
    bool failureDelivered = false;
    Require(service->SubmitCleanup(
        [owner] { throw std::runtime_error("retained cleanup failure"); },
        [&](std::exception_ptr failure) { failureDelivered = failure != nullptr; }),
        "retained cleanup failure job was rejected");
    owner.reset();
    ProcessUntil([&] { return failureDelivered; },
                 "retained cleanup failure was not delivered");
    Require(!weakOwner.expired() && destroyed->load(std::memory_order_acquire) == 0,
            "failed cleanup released ownership");
    Require(service->busy() && service->pending() == 1,
            "failed cleanup was removed from pending state");

    bool drained = false;
    bool stopped = false;
    service->DrainAsync([&] { drained = true; });
    service->ShutdownAsync([&] { stopped = true; });
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(100);
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        std::this_thread::yield();
    }
    Require(!drained && !stopped && service->busy() && service->pending() == 1,
            "failed cleanup incorrectly completed drain or shutdown");
    std::cout << "RETAINED_CLEANUP_FAILURE ownership-retained/pending=1/no-false-terminal PASS"
              << std::endl;
    std::_Exit(0);
}

void TestRetainedCleanupFailureIsProcessIsolated() {
    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({QStringLiteral("--retained-cleanup-failure-child")});
    child.start();
    Require(child.waitForStarted(5000),
            "retained cleanup child did not start");
    Require(child.waitForFinished(5000),
            "retained cleanup child did not terminate");
    Require(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
            "retained cleanup child failed");
    const auto output = child.readAllStandardOutput();
    Require(output.contains("RETAINED_CLEANUP_FAILURE") &&
                output.contains("PASS"),
            "retained cleanup child did not produce its terminal evidence");
}

void TestRuntimePostRevocationIsAtomic() {
    asio::io_context context;
    auto work = asio::make_work_guard(context);
    auto runtime = std::make_shared<OpenMeeting::MeetingSessionRuntime>(
        context, 91, QStringLiteral("post-revoke-user"));
    std::atomic<bool> start{false};
    std::atomic<int> accepted{0};
    std::atomic<int> executed{0};
    std::thread worker([&] { context.run(); });

    constexpr int kProducers = 4;
    constexpr int kPostsPerProducer = 256;
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int producer = 0; producer != kProducers; ++producer) {
        producers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int index = 0; index != kPostsPerProducer; ++index) {
                if (runtime->post([&executed] {
                        executed.fetch_add(1, std::memory_order_release);
                    })) {
                    accepted.fetch_add(1, std::memory_order_release);
                }
            }
        });
    }
    std::thread revoker([&] {
        start.store(true, std::memory_order_release);
        std::this_thread::yield();
        runtime->revokeCallbacks();
    });
    for (auto& producer : producers) producer.join();
    revoker.join();
    Require(!runtime->post([&executed] { ++executed; }),
            "revoked runtime accepted a new callback");

    std::promise<void> barrier;
    auto barrierFuture = barrier.get_future();
    asio::post(runtime->strand(), [&] { barrier.set_value(); });
    Require(barrierFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "post/revoke barrier timed out");
    Require(executed.load(std::memory_order_acquire) ==
                accepted.load(std::memory_order_acquire),
            "post/revoke race lost an accepted task or ran a rejected task");
    work.reset();
    worker.join();
}

livekit::TrackKey PolicyTrack(
        std::uint64_t native_generation,
        std::uint64_t incarnation,
        const char* sid) {
    livekit::TrackKey key;
    key.participant.native_room_generation = native_generation;
    key.participant.sid = "PA_POLICY";
    key.participant.identity = "policy-peer";
    key.participant.incarnation = 1;
    key.publication_sid = sid;
    key.publication_incarnation = incarnation;
    return key;
}

void TestVideoPolicyTelemetryShutdownCrossing() {
    asio::io_context context;
    auto runtime = std::make_shared<OpenMeeting::MeetingSessionRuntime>(
        context, 92, QStringLiteral("policy-user"));
    std::vector<livekit::telemetry::SessionTelemetry::SnapshotPtr> snapshots;
    const auto first = PolicyTrack(7, 1, "TR_POLICY_A");
    const auto second = PolicyTrack(7, 1, "TR_POLICY_B");

    asio::post(runtime->strand(), [runtime, &snapshots, first, second] {
        runtime->telemetry()->SetSnapshotCallbackOnStrand(
            [&snapshots](auto snapshot) { snapshots.push_back(std::move(snapshot)); });
        livekit::VideoDemandPlan plan;
        plan.coordinator_session = 92;
        plan.catalog_revision = 5;
        plan.policy_revision = 3;
        plan.stage_content = livekit::StageContent::Video;
        plan.reason = livekit::VideoDemandReason::Visible;
        plan.visible_seats = {{first}, {second}};
        plan.selected_video = {first};
        Require(runtime->recordAcceptedVideoDemandOnStrand(plan, 7),
                "accepted plan was not recorded");

        livekit::VideoRenderSelectionObservation rendered;
        rendered.coordinator_session = 92;
        rendered.native_room_generation = 7;
        rendered.catalog_revision = 5;
        rendered.policy_revision = 3;
        rendered.actual_video = {first, second};
        rendered.bound_video = {first};
        Require(runtime->recordVideoRenderSelectionOnStrand(rendered),
                "render convergence was not recorded");
    });
    context.run();
    Require(!snapshots.empty(), "video policy telemetry did not publish");
    auto converged = snapshots.back();
    Require(converged->video_policy_requested == 2 &&
                converged->video_policy_selected == 1 &&
                converged->video_policy_actual == 2 &&
                converged->video_policy_bound == 1,
            "requested/selected/actual/bound metrics diverged");
    Require(converged->video_policy_actual_not_selected == 1 &&
                converged->video_policy_selected_not_bound == 0,
            "video policy set differences were not preserved");

    runtime->revokeCallbacks();
    Require(!runtime->post([] {}),
            "shutdown crossing admitted a late render observation");
    context.restart();
    asio::post(runtime->strand(), [runtime] {
        runtime->stopAcceptingDataOnStrand();
        runtime->stopVideoDemandOnStrand();
        runtime->stopTelemetryOnStrand();
    });
    context.run();
    Require(snapshots.back()->session_complete &&
                snapshots.back()->video_policy_retired &&
                snapshots.back()->video_policy_requested == 0 &&
                snapshots.back()->video_policy_selected == 0 &&
                snapshots.back()->video_policy_actual == 0 &&
                snapshots.back()->video_policy_bound == 0,
            "shutdown did not publish one retired zero policy terminal");

    auto successor = std::make_shared<OpenMeeting::MeetingSessionRuntime>(
        context, 93, QStringLiteral("successor-user"));
    livekit::telemetry::SessionTelemetry::SnapshotPtr successorSnapshot;
    context.restart();
    asio::post(successor->strand(), [successor, &successorSnapshot, first] {
        livekit::VideoRenderSelectionObservation stale;
        stale.coordinator_session = 92;
        stale.native_room_generation = 7;
        stale.catalog_revision = 5;
        stale.policy_revision = 3;
        stale.actual_video = {first};
        stale.bound_video = {first};
        Require(!successor->recordVideoRenderSelectionOnStrand(stale),
                "successor accepted the old session render observation");
        successorSnapshot = successor->telemetry()->SnapshotOnStrand();
    });
    context.run();
    Require(successorSnapshot && successorSnapshot->video_policy_revision == 0 &&
                successorSnapshot->video_policy_requested == 0 &&
                successorSnapshot->video_policy_stale_updates == 1,
            "old session policy telemetry contaminated its successor");
}

void TestAsyncShutdownGuardKeepsQtResponsive(QCoreApplication& app) {
    int shutdownCalls = 0;
    bool heartbeatDelivered = false;
    bool cleanupFinished = false;
    std::function<void()> finish;
    MeetingApp::AsyncShutdownGuard guard(app, [&](std::function<void()> completed) {
        ++shutdownCalls;
        finish = std::move(completed);
        QTimer::singleShot(0, &app, [&] {
            Require(!cleanupFinished && shutdownCalls == 1,
                    "shutdown did not leave Qt responsive while cleanup was pending");
            heartbeatDelivered = true;
            QEvent repeatedQuit(QEvent::Quit);
            QCoreApplication::sendEvent(&app, &repeatedQuit);
            guard.Request(); // The production lastWindowClosed/exit-action path.
            QTimer::singleShot(0, &app, [&] {
                cleanupFinished = true;
                finish();
            });
        });
    });
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &app, [] {
        Require(false, "asynchronous quit guard did not finish");
    });
    deadline.start(5000);
    QTimer::singleShot(0, &app, [&app] {
        // Qt 5 quit() calls exit() directly; a Quit event is the interceptable
        // platform/application request handled by the guard's event filter.
        QCoreApplication::postEvent(&app, new QEvent(QEvent::Quit));
        QCoreApplication::postEvent(&app, new QEvent(QEvent::Quit));
    });
    Require(app.exec() == 0, "quit guard changed the application exit code");
    Require(shutdownCalls == 1 && heartbeatDelivered && cleanupFinished,
            "repeated Quit bypassed or repeated the pending shutdown transaction");
}

void TestAsyncShutdownGuardUnwindsExistingLoop(QCoreApplication& app) {
    QEventLoop* existingLoop = nullptr;
    bool nestedLoopReturned = false;
    bool cleanupStarted = false;
    bool cleanupFinished = false;
    int unwindCalls = 0;
    MeetingApp::AsyncShutdownGuard guard(app,
        [&](std::function<void()> completed) {
            Require(nestedLoopReturned && !existingLoop && unwindCalls == 1,
                    "shutdown retired producers before the existing modal stack unwound");
            Require(QThread::currentThread()->loopLevel() == 1,
                    "shutdown cleanup started inside the existing nested loop");
            cleanupStarted = true;
            QTimer::singleShot(0, &app, [&, completed = std::move(completed)] {
                cleanupFinished = true;
                completed();
            });
        }, [&] {
            Require(existingLoop && !nestedLoopReturned && !cleanupStarted,
                    "modal unwind ran after cleanup or outside the existing loop");
            ++unwindCalls;
            existingLoop->quit();
        });
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &app, [] {
        Require(false, "quit guard did not unwind the existing nested loop");
    });
    deadline.start(5000);
    QTimer::singleShot(0, &app, [&] {
        QEventLoop modalLoop;
        existingLoop = &modalLoop;
        QTimer::singleShot(0, &modalLoop, [&app] {
            QCoreApplication::postEvent(&app, new QEvent(QEvent::Quit));
            QCoreApplication::postEvent(&app, new QEvent(QEvent::Quit));
        });
        modalLoop.exec();
        Require(!cleanupStarted,
                "shutdown cleanup ran while the modal caller still owned its stack");
        existingLoop = nullptr;
        nestedLoopReturned = true;
    });
    Require(app.exec() == 0, "nested-loop shutdown changed the application exit code");
    Require(nestedLoopReturned && cleanupStarted && cleanupFinished && unwindCalls == 1,
            "modal shutdown failed to finish exactly one unwind and cleanup");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains(
            QStringLiteral("--retained-cleanup-failure-child"))) {
        RunRetainedCleanupFailureChild();
    }

    TestQtCallbackGate();
    TestShutdownService();
    TestShutdownDrainReentry();
    TestRetainedCleanupFailureIsProcessIsolated();
    TestRuntimePostRevocationIsAtomic();
    TestVideoPolicyTelemetryShutdownCrossing();
    TestAsyncShutdownGuardKeepsQtResponsive(app);
    TestAsyncShutdownGuardUnwindsExistingLoop(app);

    asio::io_context context;
    auto guard = asio::make_work_guard(context);
    auto runtime = std::make_shared<OpenMeeting::MeetingSessionRuntime>(context, 7, QStringLiteral("local-user"));
    Require(runtime->telemetry() && runtime->telemetry()->generation() == 7,
            "session runtime did not own generation-scoped telemetry");

    constexpr int kProducerCount = 4;
    constexpr int kTasksPerProducer = 128;
    constexpr int kTaskCount = kProducerCount * kTasksPerProducer;
    std::atomic<int> activeHandlers{0};
    std::atomic<int> completedHandlers{0};
    std::promise<void> stopped;
    auto stoppedFuture = stopped.get_future();

    std::thread workerA([&] { context.run(); });
    std::thread workerB([&] { context.run(); });

    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    for (int producer = 0; producer != kProducerCount; ++producer) {
        producers.emplace_back([&, producer] {
            for (int task = 0; task != kTasksPerProducer; ++task) {
                const int id = producer * kTasksPerProducer + task;
                asio::post(runtime->strand(), [runtime, &activeHandlers, &completedHandlers, id]() {
                    runtime->assertOnStrand();
                    Require(runtime->acceptsDataOnStrand(), "accepted task ran after stop barrier");
                    Require(activeHandlers.fetch_add(1, std::memory_order_acq_rel) == 0,
                            "strand allowed concurrent transfer mutation");

                    auto &transfers = runtime->transfersOnStrand();
                    OpenMeeting::InboundMediaTransfer transfer;
                    transfer.senderIdentity = QStringLiteral("remote-user");
                    transfer.totalChunks = 1;
                    transfers.emplace(QString::number(id), std::move(transfer));

                    std::this_thread::yield();
                    Require(activeHandlers.fetch_sub(1, std::memory_order_acq_rel) == 1,
                            "transfer mutation overlap accounting failed");
                    completedHandlers.fetch_add(1, std::memory_order_release);
                });
            }
        });
    }
    for (auto &producer : producers) {
        producer.join();
    }

    asio::post(runtime->strand(), [runtime, &completedHandlers, &stopped]() {
        runtime->assertOnStrand();
        Require(completedHandlers.load(std::memory_order_acquire) == kTaskCount,
                "stop barrier ran before already-posted transfer work");
        Require(runtime->transfersOnStrand().size() == kTaskCount,
                "serialized transfer state lost an update");
        runtime->stopAcceptingDataOnStrand();
        Require(!runtime->acceptsDataOnStrand(), "stop barrier did not reject later data");
        runtime->transfersOnStrand().clear();
        runtime->stopTelemetryOnStrand([runtime, &stopped]() {
            asio::post(runtime->strand(), [runtime, &stopped]() {
                Require(!runtime->acceptsDataOnStrand(), "post-stop task was admitted");
                Require(runtime->transfersOnStrand().empty(), "post-stop cleanup retained transfer state");
                stopped.set_value();
            });
        });
    });

    Require(stoppedFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "session strand stop barrier timed out");
    livekit::telemetry::Event postStop;
    postStop.kind = livekit::telemetry::EventKind::GaugeSample;
    postStop.session_generation = runtime->generation();
    postStop.availability = livekit::telemetry::Availability::Valid;
    Require(!runtime->telemetry()->Submit(std::move(postStop)),
            "telemetry producer was accepted after the session stop barrier");
    guard.reset();
    workerA.join();
    workerB.join();

    Require(activeHandlers.load(std::memory_order_acquire) == 0, "handler accounting did not settle");
    Require(completedHandlers.load(std::memory_order_acquire) == kTaskCount, "not all handlers completed");

    // Qt queued work must use a plain generation token, never a strong
    // MeetingSessionRuntime reference. This models a delayed UI callback
    // running after a session's ASIO context has been torn down.
    std::weak_ptr<OpenMeeting::MeetingSessionRuntime> deferredRuntimeWeak;
    std::atomic<bool> deferredCallbackRan{false};
    {
        auto deferredContext = std::make_unique<asio::io_context>();
        auto deferredRuntime = std::make_shared<OpenMeeting::MeetingSessionRuntime>(
            *deferredContext, 8, QStringLiteral("deferred-user"));
        deferredRuntimeWeak = deferredRuntime;
        const uint64_t generation = deferredRuntime->generation();

        QMetaObject::invokeMethod(&app, [&deferredCallbackRan, generation]() {
            Require(generation == 8, "queued callback lost its session generation");
            deferredCallbackRan.store(true, std::memory_order_release);
        }, Qt::QueuedConnection);

        deferredRuntime.reset();
        Require(deferredRuntimeWeak.expired(),
                "queued Qt callback retained the session runtime");
        deferredContext.reset();
    }
    QCoreApplication::processEvents();
    Require(deferredCallbackRan.load(std::memory_order_acquire),
            "queued generation callback did not run after context teardown");
    return 0;
}
