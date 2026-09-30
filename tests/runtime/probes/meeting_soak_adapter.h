#pragma once

// Opt-in, file-controlled L3 adapter. This header is only compiled into the
// existing Qt acceptance executable; it does not alter production defaults.
#include "src/core/meeting_coordinator.h"
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QLockFile>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>
#include <cmath>
#include <functional>
#include <memory>

namespace meeting_soak {

inline bool WriteJson(const QString &path, const QJsonObject &object) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()
        && file.commit();
}

inline QJsonObject ReadJson(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16384) return {};
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    return error.error == QJsonParseError::NoError && document.isObject()
        ? document.object() : QJsonObject{};
}

class Protocol final {
public:
    explicit Protocol(const QString &directory)
        : directory_(directory), lock_(QDir(directory).filePath("adapter.lock")) {}

    bool Open() {
        const QFileInfo info(directory_);
        if (!info.isAbsolute() || !info.isDir() || info.isSymLink()
            || !lock_.tryLock(0)) return false;
        const auto run = ReadJson(Path("run.json"));
        run_id_ = run.value("run_id").toString();
        return run.value("schema").toInt() == 1
            && QRegularExpression("^[A-Za-z0-9_-]{1,128}$").match(run_id_).hasMatch();
    }

    QString Path(const QString &name) const { return QDir(directory_).filePath(name); }
    QString runId() const { return run_id_; }
    qint64 commandSeq() const { return command_seq_; }
    QString commandStatus() const { return command_status_; }
    QString errorCode() const { return error_code_; }
    void Complete(bool applied, const QString &error = {}) {
        command_status_ = applied ? "applied" : "rejected";
        error_code_ = error;
    }

    QString Poll() {
        if (!QFileInfo::exists(Path("command.json"))) return {};
        const auto command = ReadJson(Path("command.json"));
        const auto number = command.value("seq").toDouble(-1);
        if (command.value("schema").toInt() != 1 || !std::isfinite(number)
            || number < 1 || number > 9007199254740991.0 || std::floor(number) != number) {
            Complete(false, "command_invalid");
            return {};
        }
        if (command.value("run_id").toString() != run_id_) {
            Complete(false, "command_run_mismatch");
            return {};
        }
        const auto seq = static_cast<qint64>(number);
        if (seq <= command_seq_) return {}; // Repeated atomic file is not a new command.
        if (seq != command_seq_ + 1) {
            Complete(false, "command_sequence_gap");
            return {};
        }
        const auto action = command.value("action").toString();
        if (command_status_ == "pending" && action != "stop") {
            Complete(false, "command_overlap");
            return {};
        }
        command_seq_ = seq;
        command_status_ = "pending";
        error_code_.clear();
        return action.isEmpty() ? QStringLiteral("invalid") : action;
    }

    bool Publish(QJsonObject object) {
        object.insert("schema", 1);
        object.insert("run_id", run_id_);
        object.insert("pid", static_cast<double>(QCoreApplication::applicationPid()));
        object.insert("heartbeat_seq", static_cast<double>(++heartbeat_seq_));
        object.insert("command_seq", static_cast<double>(command_seq_));
        object.insert("command_status", command_status_);
        if (object.value("error_code").toString().isEmpty()) object.insert("error_code", error_code_);
        return WriteJson(Path("status.json"), object);
    }

private:
    QString directory_, run_id_, command_status_ = "applied", error_code_;
    QLockFile lock_;
    qint64 command_seq_ = 0, heartbeat_seq_ = 0;
};

class PatternWindow final : public QWidget {
public:
    PatternWindow() {
        setWindowTitle("Cohavora soak owned test pattern");
        setAttribute(Qt::WA_ShowWithoutActivating);
        resize(640, 360);
        timer_.setInterval(250);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] { ++frame_; update(); });
        timer_.start();
    }
    livekit::DesktopSource source() {
        show();
        return {livekit::DesktopSourceKind::Window, static_cast<intptr_t>(winId()),
            "soak-owned-pattern"};
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), frame_ % 2 ? Qt::darkBlue : Qt::darkGreen);
        painter.fillRect(QRect((frame_ * 13) % width(), 0, 40, height()), Qt::white);
        painter.setPen(Qt::yellow);
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Soak pattern %1").arg(frame_));
    }
private:
    QTimer timer_;
    int frame_ = 0;
};

struct RenderPathProbe {
    quint64 page = 0;
    quint64 page_size = 0;
    quint64 page_count = 0;
    QString selected_fingerprint;
    QJsonArray selected_tracks;
    QJsonArray inbound_streams;
    quint64 stats_sample_seq = 0;
    quint64 stats_age_ms = 0;
    quint64 stats_video_streams = 0;
    quint64 stats_track_ids_available = 0;
    quint64 router_submitted = 0;
    quint64 router_rejected_binding = 0;
    quint64 delivered_to_gpu = 0;
    quint64 attached_tracks = 0;
    quint64 track_frames_received = 0;
    quint64 lease_rejected_frames = 0;
    quint64 selected_native_sinks = 0;
    quint64 selected_native_recent = 0;
    quint64 selected_active_leases = 0;
    quint64 duplicate_same_track = 0;
    quint64 duplicate_new_track = 0;
    bool timer_active = false;
    bool stage_visible = false;
    bool canvas_visible = false;
    bool renderer_ready = false;
};

struct Hooks {
    // Empty predicate means rejected; a true predicate means the accepted
    // production policy and UI have reached the requested state.
    std::function<std::function<bool()>(const QString &)> viewport;
    std::function<quint64()> policyRevision;
    std::function<quint64()> remoteVideoCount;
    std::function<QString()> renderBackend;
    std::function<RenderPathProbe()> renderPath;
    std::function<bool(bool, std::function<void(bool)>)> reconnect;
};

class Adapter final : public QObject {
public:
    Adapter(Protocol &protocol, OpenMeeting::MeetingCoordinator &coordinator, Hooks hooks)
        : protocol_(protocol), coordinator_(coordinator), hooks_(std::move(hooks)) {
        timer_.setInterval(1000);
        connect(&timer_, &QTimer::timeout, this, [this] { Tick(); });
        connect(&coordinator_, &OpenMeeting::MeetingCoordinator::telemetrySnapshotChanged,
            this, [this](const QVariantMap &snapshot) { telemetry_ = snapshot; });
        connect(&coordinator_, &OpenMeeting::MeetingCoordinator::stateChanged,
            this, [this](OpenMeeting::MeetingState state, const QString &) {
                if (state == OpenMeeting::MeetingState::Reconnecting) reconnect_seen_ = true;
                if (state == OpenMeeting::MeetingState::Failed) Fail("meeting_failed");
                if (state == OpenMeeting::MeetingState::Idle && connected_ && !stopping_)
                    Fail("unexpected_disconnect");
                if (state == OpenMeeting::MeetingState::InMeeting) connected_ = true;
            });
        connect(&coordinator_, &OpenMeeting::MeetingCoordinator::sessionShutdownFinished,
            this, [this] {
                shutdown_finished_ = true;
                if (!stopping_) Fail("unexpected_shutdown");
                QTimer::singleShot(0, &loop_, &QEventLoop::quit);
            });
        connect(&coordinator_, &OpenMeeting::MeetingCoordinator::errorOccurred,
            this, [this](const QString &, const QString &) { Fail("coordinator_error"); });
        connect(&coordinator_, &OpenMeeting::MeetingCoordinator::screenShareChanged,
            this, [this](livekit::ScreenShareSnapshot snapshot) {
                if (snapshot.state == livekit::ScreenShareState::Failed
                    || snapshot.state == livekit::ScreenShareState::StopFailed)
                    Fail("screen_share_failed");
            });
    }

    int Run(const std::function<void()> &start) {
        if (!PublishWithRetry()) return 3;
        timer_.start();
        QTimer::singleShot(0, this, start);
        loop_.exec();
        timer_.stop();
        return error_.isEmpty() && shutdown_finished_ ? 0 : 3;
    }

    // Called only after shutdown acknowledgement AND destruction/deinitialization
    // by the caller. The shutdown gate prevents further Qt telemetry delivery.
    bool Finalize(bool resourcesReleased) {
        stopped_ = shutdown_finished_ && resourcesReleased;
        if (!stopped_ && error_.isEmpty()) error_ = "shutdown_not_quiescent";
        if (stopped_ && error_.isEmpty()) protocol_.Complete(true);
        return PublishWithRetry();
    }

private:
    bool PublishWithRetry() {
        if (Publish()) return true;
        QEventLoop retryLoop;
        QTimer retryTimer;
        bool written = false;
        retryTimer.setInterval(100);
        connect(&retryTimer, &QTimer::timeout, &retryLoop, [&] {
            if (Publish()) { written = true; retryLoop.quit(); }
        });
        QTimer::singleShot(2000, &retryLoop, &QEventLoop::quit);
        retryTimer.start();
        retryLoop.exec();
        return written;
    }

    void Fail(const QString &code) {
        if (error_.isEmpty()) error_ = code;
        protocol_.Complete(false, code);
        if (!stopping_) {
            stopping_ = true;
            QTimer::singleShot(0, this, [this] { coordinator_.leaveMeetingAsync(false); });
        }
    }

    QString State() const {
        if (!error_.isEmpty()) return "failed";
        if (stopped_) return "stopped";
        if (stopping_) return "stopping";
        const auto state = coordinator_.state();
        if (state == OpenMeeting::MeetingState::InMeeting) return "connected";
        if (state == OpenMeeting::MeetingState::Reconnecting) return "reconnecting";
        return "connecting";
    }

    bool Publish() {
        const auto count = [this](const char *key) { return static_cast<double>(telemetry_.value(key).toULongLong()); };
        const auto render = hooks_.renderPath ? hooks_.renderPath() : RenderPathProbe{};
        QJsonObject status{
            {"state", State()}, {"runtime_seq", count("revision")},
            {"policy_revision", count("videoPolicyRevision")},
            {"requested", stopped_ ? 0 : count("videoPolicyRequested")},
            {"selected", stopped_ ? 0 : count("videoPolicySelected")},
            {"actual", stopped_ ? 0 : count("videoPolicyActual")},
            {"bound", stopped_ ? 0 : count("videoPolicyBound")},
            {"selected_not_bound", stopped_ ? 0 : count("videoPolicySelectedNotBound")},
            {"bound_not_selected", stopped_ ? 0 : count("videoPolicyBoundNotSelected")},
            {"remote_video_count", stopped_ ? 0 : static_cast<double>(hooks_.remoteVideoCount())},
            {"render_submits", count("uniqueRenderSubmits")},
            {"decoded_frames", count("inboundVideoFramesDecoded")},
            {"render_expected_bindings", count("renderExpectedBindings")},
            {"render_hidden_bindings", count("renderHiddenBindings")},
            {"render_router_submitted", static_cast<double>(render.router_submitted)},
            {"render_router_rejected_binding", static_cast<double>(render.router_rejected_binding)},
            {"render_delivered_to_gpu", static_cast<double>(render.delivered_to_gpu)},
            {"render_attached_tracks", static_cast<double>(render.attached_tracks)},
            {"page", static_cast<double>(render.page)},
            {"page_size", static_cast<double>(render.page_size)},
            {"page_count", static_cast<double>(render.page_count)},
            {"selected_fingerprint", render.selected_fingerprint},
            {"selected_tracks", render.selected_tracks},
            {"inbound_streams", render.inbound_streams},
            {"stats_sample_seq", static_cast<double>(render.stats_sample_seq)},
            {"stats_age_ms", static_cast<double>(render.stats_age_ms)},
            {"stats_video_streams", static_cast<double>(render.stats_video_streams)},
            {"stats_track_ids_available", static_cast<double>(render.stats_track_ids_available)},
            {"track_frames_received", static_cast<double>(render.track_frames_received)},
            {"lease_rejected_frames", static_cast<double>(render.lease_rejected_frames)},
            {"selected_native_sinks", static_cast<double>(render.selected_native_sinks)},
            {"selected_native_recent", static_cast<double>(render.selected_native_recent)},
            {"selected_active_leases", static_cast<double>(render.selected_active_leases)},
            {"duplicate_same_track", static_cast<double>(render.duplicate_same_track)},
            {"duplicate_new_track", static_cast<double>(render.duplicate_new_track)},
            {"render_timer_active", render.timer_active},
            {"video_stage_visible", render.stage_visible},
            {"canvas_visible", render.canvas_visible},
            {"renderer_ready", render.renderer_ready},
            {"audio_frames", QJsonValue(QJsonValue::Null)},
            {"sharing", !stopped_ && coordinator_.screenShareSnapshot().state == livekit::ScreenShareState::Active},
            {"render_backend", stopped_ ? QStringLiteral("released") : hooks_.renderBackend()},
            {"policy_measurement_point", stopped_ ? QStringLiteral("shutdown_quiescence") : QStringLiteral("session_video_policy_convergence")},
            {"error_code", error_}
        };
        return protocol_.Publish(std::move(status));
    }

    void Tick() {
        const auto action = protocol_.Poll();
        if (!action.isEmpty()) Execute(action);
        if (protocol_.commandStatus() == "pending" && pending_ && pending_()) {
            protocol_.Complete(true);
            pending_ = {};
        }
        // Windows readers may briefly deny rename/delete sharing. Keep the
        // previous complete document and retry next tick; the parent watchdog
        // detects a persistently unwritable/stalled status channel.
        Publish();
    }

    void Execute(const QString &action) {
        pending_ = {};
        if (action == "stop") {
            stopping_ = true;
            coordinator_.leaveMeetingAsync(false);
            return;
        }
        if (stopping_ || coordinator_.state() != OpenMeeting::MeetingState::InMeeting) {
            protocol_.Complete(false, "meeting_not_ready");
            return;
        }
        if (action == "share_start") {
            if (!pattern_) pattern_ = std::make_unique<PatternWindow>();
            coordinator_.startScreenShare(pattern_->source());
            pending_ = [this] { return coordinator_.screenShareSnapshot().state == livekit::ScreenShareState::Active; };
        } else if (action == "share_stop") {
            coordinator_.stopScreenShare();
            pending_ = [this] {
                if (coordinator_.screenShareSnapshot().state != livekit::ScreenShareState::Idle) return false;
                if (pattern_) pattern_->hide();
                return true;
            };
        } else if (action == "soft_reconnect" || action == "full_reconnect") {
            reconnect_seen_ = false;
            reconnect_completed_ = false;
            const auto seq = protocol_.commandSeq();
            QPointer<Adapter> guard(this);
            if (!hooks_.reconnect(action == "full_reconnect", [guard, seq](bool ok) {
                if (!guard || guard->protocol_.commandSeq() != seq || guard->stopping_) return;
                if (!ok) guard->protocol_.Complete(false, "reconnect_request_failed");
                else guard->reconnect_completed_ = true;
            })) {
                protocol_.Complete(false, "reconnect_unavailable");
                return;
            }
            pending_ = [this] {
                return reconnect_seen_ && reconnect_completed_
                    && coordinator_.state() == OpenMeeting::MeetingState::InMeeting;
            };
        } else {
            const auto applied = hooks_.viewport(action);
            if (!applied) { protocol_.Complete(false, "action_unavailable"); return; }
            pending_ = [this, applied] {
                return applied() && telemetry_.value("videoPolicyRevision").toULongLong() >= hooks_.policyRevision();
            };
        }
    }

    Protocol &protocol_;
    OpenMeeting::MeetingCoordinator &coordinator_;
    Hooks hooks_;
    QTimer timer_;
    QEventLoop loop_;
    QVariantMap telemetry_;
    std::function<bool()> pending_;
    std::unique_ptr<PatternWindow> pattern_;
    QString error_;
    bool connected_ = false, stopping_ = false, stopped_ = false;
    bool shutdown_finished_ = false, reconnect_seen_ = false, reconnect_completed_ = false;
};

inline bool ProtocolSelfTest() {
    QTemporaryDir directory;
    if (!directory.isValid() || !WriteJson(directory.filePath("run.json"), {{"schema", 1}, {"run_id", "selftest"}})) return false;
    Protocol protocol(directory.path());
    if (!protocol.Open()) return false;
    Protocol duplicate(directory.path());
    if (duplicate.Open()) return false;
    const auto send = [&](int seq, const QString &run, const QString &action) {
        return WriteJson(directory.filePath("command.json"), {{"schema", 1}, {"run_id", run}, {"seq", seq}, {"action", action}});
    };
    if (!send(1, "wrong", "grid9") || !protocol.Poll().isEmpty() || protocol.commandSeq() != 0) return false;
    if (!send(1, "selftest", "grid9") || protocol.Poll() != "grid9" || protocol.commandStatus() != "pending") return false;
    protocol.Complete(true);
    if (!protocol.Poll().isEmpty() || !send(3, "selftest", "pin") || !protocol.Poll().isEmpty()) return false;
    if (!send(2, "selftest", "stop") || protocol.Poll() != "stop") return false;
    QEventLoop loop;
    bool written = false;
    QTimer::singleShot(0, &loop, [&] { written = protocol.Publish({{"state", "stopped"}, {"runtime_seq", 0}}); loop.quit(); });
    loop.exec();
    const auto status = ReadJson(directory.filePath("status.json"));
    return written && status.value("heartbeat_seq").toInt() == 1
        && status.value("runtime_seq").toInt() == 0 && status.value("command_seq").toInt() == 2;
}

} // namespace meeting_soak
