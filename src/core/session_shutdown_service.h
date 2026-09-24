#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QMetaObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QThread>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <objbase.h>
#endif

namespace OpenMeeting {
namespace detail {

// The application outlives normal asynchronous shutdown. Closing this endpoint
// at aboutToQuit also makes a retained gate harmless after the application dies.
// No worker ever reads a QPointer or addresses a short-lived QObject directly.
class QtDispatchEndpoint final {
public:
    static std::shared_ptr<QtDispatchEndpoint> Create() {
        auto* application = QCoreApplication::instance();
        Q_ASSERT(application && QThread::currentThread() == application->thread());
        if (!application || QThread::currentThread() != application->thread()) {
            return {};
        }
        auto endpoint = std::shared_ptr<QtDispatchEndpoint>(
            new QtDispatchEndpoint(application));
        const std::weak_ptr<QtDispatchEndpoint> weak(endpoint);
        QObject::connect(application, &QCoreApplication::aboutToQuit,
            application, [weak] {
                if (auto current = weak.lock()) current->Close();
            }, Qt::DirectConnection);
        QObject::connect(application, &QObject::destroyed, [weak] {
            if (auto current = weak.lock()) current->Close();
        });
        return endpoint;
    }

    template <typename F>
    bool Post(F&& callback) {
        std::lock_guard lock(mutex_);
        if (!application_) return false;
        return QMetaObject::invokeMethod(application_, std::forward<F>(callback),
                                         Qt::QueuedConnection);
    }

private:
    explicit QtDispatchEndpoint(QCoreApplication* application)
        : application_(application) {}

    void Close() {
        std::lock_guard lock(mutex_);
        application_ = nullptr;
    }

    std::mutex mutex_;
    QCoreApplication* application_ = nullptr;
};

} // namespace detail

// Create and destroy the receiver on the application thread. Post and Revoke
// may be called from any thread. Revoke suppresses queued/future delivery, but
// does not interrupt a callback which is already executing on the Qt thread.
template <typename T>
class QtCallbackGate final {
public:
    static std::shared_ptr<QtCallbackGate> Create(T* receiver) {
        auto endpoint = detail::QtDispatchEndpoint::Create();
        Q_ASSERT(receiver && endpoint &&
                 receiver->thread() == QCoreApplication::instance()->thread());
        if (!receiver || !endpoint ||
            receiver->thread() != QCoreApplication::instance()->thread()) {
            return {};
        }
        auto gate = std::shared_ptr<QtCallbackGate>(
            new QtCallbackGate(receiver, std::move(endpoint)));
        const std::weak_ptr<State> weak(gate->state_);
        QObject::connect(receiver, &QObject::destroyed, [weak] {
            if (auto state = weak.lock()) {
                state->active.store(false, std::memory_order_release);
            }
        });
        return gate;
    }

    bool active() const {
        return state_->active.load(std::memory_order_acquire);
    }

    void Revoke() {
        state_->active.store(false, std::memory_order_release);
    }

    template <typename F>
    bool Post(F&& callback) const {
        auto state = state_;
        if (!state->active.load(std::memory_order_acquire)) return false;
        return state->endpoint->Post(
            [state, callback = std::forward<F>(callback)]() mutable {
                if (!state->active.load(std::memory_order_acquire)) return;
                // QPointer is inspected only after entering the application
                // thread; QObject destruction cannot race this receiver lookup.
                auto* receiver = state->receiver.data();
                if (receiver) callback(receiver);
            });
    }

private:
    struct State {
        State(T* target, std::shared_ptr<detail::QtDispatchEndpoint> dispatcher)
            : receiver(target), endpoint(std::move(dispatcher)) {}
        QPointer<T> receiver;
        std::shared_ptr<detail::QtDispatchEndpoint> endpoint;
        std::atomic_bool active{true};
    };

    QtCallbackGate(T* receiver,
                   std::shared_ptr<detail::QtDispatchEndpoint> endpoint)
        : state_(std::make_shared<State>(receiver, std::move(endpoint))) {}

    std::shared_ptr<State> state_;
};

// An application-owned queue for native cleanup and joining retired I/O threads.
// Initialize on the application thread. Before normal application exit, call
// ShutdownAsync and keep the Qt event loop alive until its callback runs.
// Callbacks and destruction run on the application thread; Submit/busy/pending
// are thread safe. Jobs must not synchronously wait for a Qt callback.
class SessionShutdownService final {
public:
    using Completion = std::function<void(std::exception_ptr)>;

    static SessionShutdownService& Instance() {
        static SessionShutdownService service;
        return service;
    }

    SessionShutdownService()
        : state_(std::make_shared<State>()) {
        state_->endpoint = detail::QtDispatchEndpoint::Create();
        Q_ASSERT(state_->endpoint);
        state_->worker = std::thread([state = state_] { Run(state); });
    }

    ~SessionShutdownService() {
        auto state = state_;
        {
            std::lock_guard lock(state->mutex);
            state->accepting = false;
            state->stopping = true;
        }
        state->wake.notify_one();
        // Last-resort process/static teardown retains ownership and waits for
        // safe cleanup, even if QCoreApplication was destroyed first. Normal
        // production shutdown joins through ShutdownAsync after jobs finish.
        Join(state);
    }

    SessionShutdownService(const SessionShutdownService&) = delete;
    SessionShutdownService& operator=(const SessionShutdownService&) = delete;

    bool Submit(std::function<void()> job, Completion completed = {}) {
        if (!job) return false;
        auto state = state_;
        {
            std::lock_guard lock(state->mutex);
            if (!state->accepting) return false;
            state->jobs.push_back({std::move(job), std::move(completed), false});
            ++state->pending;
        }
        state->wake.notify_one();
        return true;
    }

    // Ownership-bearing cleanup may arrive while an earlier Qt receiver is
    // being destroyed during drain. Accept it until worker exit is committed.
    // After that point the application violated the shutdown ordering contract;
    // fail before destroying the native owner on the caller's (possibly UI)
    // thread. Revoke/retire all producers before asking the service to stop.
    bool SubmitCleanup(std::function<void()> job, Completion completed = {}) {
        if (!job) return false;
        auto state = state_;
        {
            std::lock_guard lock(state->mutex);
            if (state->workerExiting) std::terminate();
            state->jobs.push_back({std::move(job), std::move(completed), true});
            ++state->pending;
        }
        state->wake.notify_one();
        return true;
    }

    std::size_t pending() const {
        std::lock_guard lock(state_->mutex);
        return state_->pending;
    }

    bool busy() const { return pending() != 0; }

    // Observes queue quiescence without closing admission. If an earlier Qt
    // completion submits another job, the drain callback waits for that job too.
    void DrainAsync(std::function<void()> onDrained) {
        AssertOnApplicationThread();
        if (!onDrained) return;
        auto state = state_;
        bool idle = false;
        {
            std::lock_guard lock(state->mutex);
            state->drains.push_back(std::move(onDrained));
            idle = state->pending == 0;
        }
        if (idle) Post(state, [state] { DeliverDrains(state); });
    }

    // Closes admission once, drains accepted jobs, and joins only after the
    // worker has left the job loop. Repeated calls receive the same completion.
    void ShutdownAsync(std::function<void()> onStopped = {}) {
        AssertOnApplicationThread();
        auto state = state_;
        bool exited = false;
        {
            std::lock_guard lock(state->mutex);
            state->accepting = false;
            state->stopping = true;
            if (onStopped) state->stopped.push_back(std::move(onStopped));
            exited = state->workerExited;
        }
        state->wake.notify_one();
        if (exited) Post(state, [state] { DeliverStopped(state); });
    }

private:
    struct Job {
        std::function<void()> run;
        Completion completed;
        bool retainOnFailure = false;
    };

    struct State {
        std::shared_ptr<detail::QtDispatchEndpoint> endpoint;
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<Job> jobs;
        // A failed native job may still own a joinable thread. Keep its exact
        // captures alive and its pending count outstanding; never claim a
        // successful drain or permit WebRTC teardown over those resources.
        std::vector<std::function<void()>> failedJobs;
        std::deque<std::function<void()>> drains;
        std::deque<std::function<void()>> stopped;
        std::size_t pending = 0;
        bool accepting = true;
        bool stopping = false;
        bool workerExiting = false;
        bool workerExited = false;
        std::thread worker;
    };

    static void AssertOnApplicationThread() {
        Q_ASSERT(QCoreApplication::instance() && QThread::currentThread() ==
                 QCoreApplication::instance()->thread());
    }

    template <typename F>
    static void Post(const std::shared_ptr<State>& state, F&& callback) {
        if (state->endpoint) state->endpoint->Post(std::forward<F>(callback));
    }

    static void Join(const std::shared_ptr<State>& state) {
        if (!state->worker.joinable()) return;
        // Destroying the service from one of its jobs violates ownership.
        if (state->worker.get_id() == std::this_thread::get_id()) std::terminate();
        state->worker.join();
    }

    static void DeliverDrains(const std::shared_ptr<State>& state) {
        for (;;) {
            std::function<void()> callback;
            {
                std::lock_guard lock(state->mutex);
                if (state->pending != 0 || state->drains.empty()) return;
                callback = std::move(state->drains.front());
                state->drains.pop_front();
            }
            callback();
        }
    }

    static void DeliverStopped(const std::shared_ptr<State>& state) {
        {
            std::lock_guard lock(state->mutex);
            if (!state->workerExited) return;
        }
        Join(state);
        DeliverDrains(state);
        for (;;) {
            std::function<void()> callback;
            {
                std::lock_guard lock(state->mutex);
                if (state->stopped.empty()) return;
                callback = std::move(state->stopped.front());
                state->stopped.pop_front();
            }
            callback();
        }
    }

    static void Run(const std::shared_ptr<State>& state) {
#if defined(_WIN32)
        const auto comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
        for (;;) {
            Job job;
            {
                std::unique_lock lock(state->mutex);
                state->wake.wait(lock, [&] {
                    return !state->jobs.empty() || (state->stopping && state->pending == 0);
                });
                if (state->jobs.empty()) {
                    // Mark this while still holding the admission mutex, so a
                    // late cleanup cannot enter between the empty check/exit.
                    state->workerExiting = true;
                    break;
                }
                job = std::move(state->jobs.front());
                state->jobs.pop_front();
            }
            std::exception_ptr failure;
            try {
                job.run();
            } catch (...) {
                failure = std::current_exception();
            }
            const bool retained = failure && job.retainOnFailure;
            if (retained) {
                {
                    std::lock_guard lock(state->mutex);
                    state->failedJobs.push_back(std::move(job.run));
                }
                qCritical() << "Native cleanup failed; retaining resources and keeping shutdown pending.";
            } else {
                // Destroy native owners before reporting quiescence. Their
                // destructors can themselves perform cleanup.
                job.run = {};
            }
            if (job.completed) {
                Post(state, [callback = std::move(job.completed), failure] {
                    callback(failure);
                });
            }
            bool idle = false;
            {
                std::lock_guard lock(state->mutex);
                if (!retained) --state->pending;
                idle = state->pending == 0;
            }
            if (idle) Post(state, [state] { DeliverDrains(state); });
        }
#if defined(_WIN32)
        if (SUCCEEDED(comResult)) CoUninitialize();
#endif
        {
            std::lock_guard lock(state->mutex);
            state->workerExited = true;
        }
        Post(state, [state] { DeliverStopped(state); });
    }

    std::shared_ptr<State> state_;
};

} // namespace OpenMeeting
