#pragma once

#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
#include "rtc/webrtc_manager.h"
#include "core/session_shutdown_service.h"
#include "telemetry/diagnostic_pipeline.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QStringList>
#include <QtCore/QTimer>

namespace livekit::detail {

// Application-thread owner for Linux discovery requests. The worker never
// addresses a QObject; closing the application revokes queued completions.
// Directory events only compare machine-local inventory, never call a driver.
class LinuxEncoderCapabilityController final : public QObject {
public:
    static LinuxEncoderCapabilityController* Instance() {
        auto* application = QCoreApplication::instance();
        Q_ASSERT(application && QThread::currentThread() == application->thread());
        if (!application || QThread::currentThread() != application->thread()) return nullptr;
        static QPointer<LinuxEncoderCapabilityController> instance;
        if (!instance) instance = new LinuxEncoderCapabilityController(application);
        return instance;
    }

    void Request(bool invalidate = false) {
        if (stopped_) return;
        if (invalidate) InvalidateVideoEncoderCapabilities();
        const auto snapshot = GetVideoEncoderCapabilities();
        if (snapshot.ready || snapshot.probing) return;
        requested_ = true;
        if (!running_) debounce_.start();
    }

private:
    explicit LinuxEncoderCapabilityController(QObject* parent)
        : QObject(parent), callbacks_(OpenMeeting::QtCallbackGate<LinuxEncoderCapabilityController>::Create(this)) {
        debounce_.setSingleShot(true);
        debounce_.setInterval(100);
        connect(&debounce_, &QTimer::timeout, this, [this] { Start(); });
        const auto changed = [this](const QString&) {
            const auto inventory = Inventory();
            WatchInventory();
            if (inventory != inventory_) {
                inventory_ = inventory;
                Request(true);
            }
        };
        connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, changed);
        connect(&watcher_, &QFileSystemWatcher::fileChanged, this, changed);
        inventory_ = Inventory();
        WatchInventory();
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
            stopped_ = true;
            debounce_.stop();
            callbacks_->Revoke();
        });
    }

    static QStringList DevicePaths() {
        QStringList result;
        const QDir render(QStringLiteral("/dev/dri"));
        for (const auto& name : render.entryList({QStringLiteral("renderD*")}, QDir::System | QDir::Files))
            result.push_back(render.filePath(name));
        const QDir devices(QStringLiteral("/dev"));
        for (const auto& name : devices.entryList({QStringLiteral("nvidia*")}, QDir::System | QDir::Files))
            result.push_back(devices.filePath(name));
        result.sort();
        return result;
    }

    static QStringList Inventory() {
        QStringList result;
        for (const auto& path : DevicePaths()) {
            const QFileInfo file(path);
            result.push_back(path + QLatin1Char('|') + file.canonicalFilePath() +
                QLatin1Char('|') + QString::number(static_cast<int>(file.permissions())));
        }
        // Symlink targets detect a render-node name being reused by another
        // PCI device. These paths stay internal to discovery, outside safe UI.
        const QDir drm(QStringLiteral("/sys/class/drm"));
        for (const auto& name : drm.entryList({QStringLiteral("renderD*")}, QDir::Dirs | QDir::System | QDir::NoDotAndDotDot))
            result.push_back(QFileInfo(drm.filePath(name)).canonicalFilePath());
        return result;
    }

    void WatchInventory() {
        QStringList desired;
        for (const auto& path : {QStringLiteral("/dev"), QStringLiteral("/dev/dri"), QStringLiteral("/sys/class/drm")})
            if (QFileInfo::exists(path)) desired.push_back(path);
        desired.append(DevicePaths());
        const auto watched = watcher_.directories() + watcher_.files();
        QStringList removed;
        for (const auto& path : watched) if (!desired.contains(path)) removed.push_back(path);
        for (const auto& path : watched) desired.removeAll(path);
        if (!removed.isEmpty()) watcher_.removePaths(removed);
        if (!desired.isEmpty()) watcher_.addPaths(desired);
    }

    void Start() {
        if (stopped_ || running_ || !requested_) return;
        requested_ = false;
        running_ = true;
        const auto gate = callbacks_;
        if (!OpenMeeting::SessionShutdownService::Instance().Submit([gate] {
                try { ProbeVideoEncoderCapabilities(); } catch (...) {}
                gate->Post([](LinuxEncoderCapabilityController* controller) {
                    controller->running_ = false;
                    const auto snapshot = GetVideoEncoderCapabilities();
                    if (snapshot.ready) {
                        diagnostic::EmitBusinessEvent(diagnostic::Event::EncoderCapability(0x10de,
                            snapshot.nvenc.h264, snapshot.nvenc.h265));
                        diagnostic::EmitBusinessEvent(diagnostic::Event::EncoderCapability(0x8086,
                            snapshot.qsv.h264, snapshot.qsv.h265));
                        // VAAPI's API identity is not synonymous with AMD AMF.
                        diagnostic::EmitBusinessEvent(diagnostic::Event::EncoderCapability(0,
                            snapshot.vaapi.h264, snapshot.vaapi.h265));
                    }
                    if (controller->requested_) controller->debounce_.start();
                });
            })) running_ = false;
    }

    QFileSystemWatcher watcher_;
    QTimer debounce_;
    std::shared_ptr<OpenMeeting::QtCallbackGate<LinuxEncoderCapabilityController>> callbacks_;
    QStringList inventory_;
    bool requested_ = false;
    bool running_ = false;
    bool stopped_ = false;
};

inline void RequestLinuxVideoEncoderCapabilityRefresh(bool invalidate = false) {
    if (auto* controller = LinuxEncoderCapabilityController::Instance()) controller->Request(invalidate);
}

} // namespace livekit::detail
#endif
