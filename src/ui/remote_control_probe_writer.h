#pragma once

#include "src/telemetry/media_diagnostic_config.h"

#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
#include <QtCore/QByteArray>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace MeetingUI {

// Diagnostic-only latest-value output. Sinks may block, so their captures must
// own only values or shared worker state, never a QObject or a borrowed owner.
class RemoteControlProbeWriter final {
public:
    using Sink = std::function<void(const QByteArray&)>;
    static constexpr int MaximumSnapshotBytes = 16 * 1024;

    static std::unique_ptr<RemoteControlProbeWriter> Create(Sink sink) noexcept {
        bool available = false;
        if (!sink || !processWorkerActive_.compare_exchange_strong(available, true)) return {};
        std::shared_ptr<Context> context;
        try {
            context = std::make_shared<Context>(std::move(sink));
            auto writer = std::unique_ptr<RemoteControlProbeWriter>(new RemoteControlProbeWriter(context));
            writer->worker_ = std::thread([context] { Run(context); });
            return writer;
        } catch (...) {
            if (!context) processWorkerActive_.store(false);
            return {};
        }
    }

    ~RemoteControlProbeWriter() { Close(); }
    RemoteControlProbeWriter(const RemoteControlProbeWriter&) = delete;
    RemoteControlProbeWriter& operator=(const RemoteControlProbeWriter&) = delete;

    bool TrySubmit(QByteArray snapshot) noexcept {
        if (snapshot.size() > MaximumSnapshotBytes || context_->stopping.load()) return false;
        std::unique_lock lock(context_->mutex, std::try_to_lock);
        if (!lock || context_->stopping.load()) return false;
        // One in-flight value and one replaceable pending value; no FIFO growth.
        context_->pending = std::move(snapshot);
        context_->wake.notify_one();
        return true;
    }

    void Close() noexcept {
        context_->stopping.store(true);
        context_->wake.notify_one();
        // The context and any active sink belong to the worker until it exits.
        // Do not acquire its mutex or join an uninterruptible filesystem call.
        if (worker_.joinable()) worker_.detach();
    }

private:
    friend struct RemoteControlProbeWriterInspectionAccess;
    struct Context {
        explicit Context(Sink output) : sink(std::move(output)) {}
        ~Context() { processWorkerActive_.store(false); }
        const Sink sink;
        std::mutex mutex;
        std::condition_variable wake;
        std::optional<QByteArray> pending;
        std::atomic_bool stopping{false};
    };
    explicit RemoteControlProbeWriter(std::shared_ptr<Context> context)
        : context_(std::move(context)) {}
    static void Run(const std::shared_ptr<Context>& context) noexcept {
        for (;;) {
            std::optional<QByteArray> snapshot;
            {
                std::unique_lock lock(context->mutex);
                // Close deliberately never locks. A timeout also covers a
                // notification racing with entry into wait after the stop flag.
                context->wake.wait_for(lock, std::chrono::milliseconds(100), [&] {
                    return context->stopping.load() || context->pending.has_value();
                });
                if (context->stopping.load()) return;
                if (!context->pending) continue;
                snapshot.swap(context->pending);
            }
            if (context->stopping.load()) return;
            try { context->sink(*snapshot); } catch (...) { }
        }
    }

    // A permanently hung sink occupies the sole process permit. Later UI
    // instances decline diagnostics instead of accumulating detached workers.
    inline static std::atomic_bool processWorkerActive_{false};
    const std::shared_ptr<Context> context_;
    std::thread worker_;
};

} // namespace MeetingUI
#endif
