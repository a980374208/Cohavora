// Phase2A1 delivered test copy; original input remains immutable and untracked.
// Original: tests/test_meeting_session_runtime.cpp
// Original SHA256: 163325d59fc2ff2de2488c48a2ba9a6caf01b0a803526fb3648605fb9fb25a4b
// Provenance and exact adaptations: tests/remediation/restored/PROVENANCE.json
// Adaptation: provenance comments only; original active checks retained.

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
