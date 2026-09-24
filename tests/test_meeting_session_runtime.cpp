#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include <QtCore/QCoreApplication>
#include <QtCore/QMetaObject>

#include "src/core/meeting_session_runtime.h"

namespace {

void Require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::abort();
    }
}

struct CatalogEventFixture {
    std::shared_ptr<livekit::MembershipState> participant;
    std::shared_ptr<livekit::TrackMembershipState> track;
    livekit::ParticipantEvent event;
};

CatalogEventFixture RemoteVideoEvent(uint64_t nativeGeneration,
                                     uint64_t incarnation,
                                     std::string trackSid) {
    CatalogEventFixture result;
    livekit::ParticipantKey participantKey{
        nativeGeneration, incarnation, "PA_RUNTIME", "runtime-peer"};
    livekit::TrackKey trackKey{participantKey, incarnation, std::move(trackSid)};
    result.participant =
        std::make_shared<livekit::MembershipState>(participantKey);
    result.track = std::make_shared<livekit::TrackMembershipState>(trackKey);
    result.event.kind = livekit::ParticipantEventKind::Upsert;
    result.event.native_room_generation = nativeGeneration;
    result.event.participant.key = participantKey;
    result.event.participant.ticket = result.participant;
    result.event.participant.state.sid = participantKey.sid;
    result.event.participant.state.identity = participantKey.identity;
    result.event.participant.state.name = "Runtime peer";
    livekit::PublicationSnapshotEvent publication;
    publication.key = trackKey;
    publication.ticket = result.track;
    publication.state.sid = trackKey.publication_sid;
    publication.state.name = "Runtime camera";
    publication.state.kind = livekit::TrackKind::Video;
    publication.state.source = livekit::TrackSource::Camera;
    publication.state.subscription_allowed = true;
    result.event.participant.publications.push_back(std::move(publication));
    return result;
}

} // namespace

int main() {
    int argc = 1;
    char applicationName[] = "test_meeting_session_runtime";
    char *argv[] = {applicationName, nullptr};
    QCoreApplication app(argc, argv);

    asio::io_context context;
    auto guard = asio::make_work_guard(context);
    auto runtime = std::make_shared<OpenMeeting::MeetingSessionRuntime>(context, 7, QStringLiteral("local-user"));

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

        auto catalog = RemoteVideoEvent(11, 1, "TR_RUNTIME");
        Require(!runtime->hasViewportIntentOnStrand(),
                "runtime reported a viewport before the first UI intent");
        Require(runtime->updatePublicationCatalogOnStrand(catalog.event) ==
                    livekit::CatalogApplyResult::Applied,
                "runtime catalog did not accept the initial generation");
        livekit::ViewportIntent viewport;
        viewport.coordinator_session = runtime->generation();
        viewport.view_revision = 1;
        viewport.catalog_revision =
            runtime->publicationCatalogOnStrand().catalog_revision;
        viewport.mode = livekit::VideoLayoutMode::Auto;
        viewport.stage_rect = {0, 0, 640, 360};
        Require(runtime->updateViewportIntentOnStrand(viewport),
                "runtime viewport was not accepted");
        Require(runtime->hasViewportIntentOnStrand(),
                "runtime did not remember the accepted UI viewport");
        const auto projected = runtime->buildRemoteMediaPlanOnStrand();
        Require(projected.coordinator_session == 7 &&
                    projected.native_room_generation == 11 &&
                    projected.known_publications.size() == 1 &&
                    projected.known_publications.front().publication_sid ==
                        "TR_RUNTIME" &&
                    projected.video.size() == 1 &&
                    projected.video.front().key.publication_sid == "TR_RUNTIME" &&
                    projected.video.front().subscribed &&
                    projected.video.front().enabled &&
                    projected.video.front().width == 320 &&
                    projected.video.front().height == 360 &&
                    projected.video.front().max_fps == 30,
                "runtime did not project complete video settings");

        livekit::RemoteMediaRecoveryRequest immediateRequest{
            7, 11, projected.catalog_revision, 1, "recovery-current"};
        bool immediate = false;
        runtime->requestRemoteMediaRecoveryOnStrand(
            immediateRequest,
            [&](livekit::RemoteMediaPlan plan) {
                immediate = plan.native_room_generation == 11 &&
                    plan.recovery_epoch == 1 &&
                    plan.recovery_token == "recovery-current";
            });
        Require(immediate, "current-generation recovery was not completed immediately");

        livekit::RemoteMediaRecoveryRequest deferredRequest{
            7, 12, projected.catalog_revision, 2, "recovery-successor"};
        bool deferred = false;
        runtime->requestRemoteMediaRecoveryOnStrand(
            deferredRequest,
            [&](livekit::RemoteMediaPlan plan) {
                deferred = plan.native_room_generation == 12 &&
                    plan.video.size() == 1 &&
                    plan.video.front().key.participant.native_room_generation == 12 &&
                    plan.recovery_epoch == 2 &&
                    plan.recovery_token == "recovery-successor";
            });
        Require(!deferred,
                "successor recovery completed before its catalog generation arrived");
        auto successor = RemoteVideoEvent(12, 2, "TR_RUNTIME");
        Require(runtime->updatePublicationCatalogOnStrand(successor.event) ==
                    livekit::CatalogApplyResult::Applied,
                "runtime catalog did not accept the successor generation");
        Require(deferred,
                "successor recovery did not complete after catalog alignment");

        runtime->stopAcceptingDataOnStrand();
        Require(!runtime->acceptsDataOnStrand(), "stop barrier did not reject later data");
        runtime->transfersOnStrand().clear();
        asio::post(runtime->strand(), [runtime, &stopped]() {
            Require(!runtime->acceptsDataOnStrand(), "post-stop task was admitted");
            Require(runtime->transfersOnStrand().empty(), "post-stop cleanup retained transfer state");
            stopped.set_value();
        });
    });

    Require(stoppedFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "session strand stop barrier timed out");
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
