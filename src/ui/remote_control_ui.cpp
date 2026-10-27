#include "remote_control_ui.h"
#include "src/core/meeting_coordinator.h"
#include "src/ui/app_theme.h"
#include "src/ui/native_child_geometry.h"
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
#include "remote_control_probe_writer.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <iterator>
#ifdef _WIN32
#include <windows.h>
#endif
#endif
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace MeetingUI {
namespace rc = livekit::remote_control;
namespace {
bool SelectableTarget(const RemoteControlTarget& target) {
    return target.widget && target.widget->isVisible() && target.content.width() > 1 && target.content.height() > 1;
}
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
const char* ProbeState(rc::State state) {
    switch (state) {
    case rc::State::Idle: return "idle";
    case rc::State::Requesting: return "requesting";
    case rc::State::AwaitingConsent: return "awaiting_consent";
    case rc::State::AwaitingReady: return "awaiting_ready";
    case rc::State::Controlling: return "controlling";
    case rc::State::Controlled: return "controlled";
    case rc::State::AwaitingActivation: return "awaiting_activation";
    }
    return "unknown";
}
const char* ProbeReason(rc::Reason reason) {
    switch (reason) {
    case rc::Reason::None: return "none";
    case rc::Reason::Declined: return "declined";
    case rc::Reason::Cancelled: return "cancelled";
    case rc::Reason::Timeout: return "timeout";
    case rc::Reason::Unavailable: return "unavailable";
    case rc::Reason::PeerChanged: return "peer_changed";
    case rc::Reason::InputStopped: return "input_stopped";
    case rc::Reason::Transport: return "transport";
    case rc::Reason::Protocol: return "protocol";
    case rc::Reason::Busy: return "busy";
    }
    return "unknown";
}
#ifdef _WIN32
struct ProbeMonitor {
    QString name;
    RECT physical{};
    bool found = false;
};
struct ProbePhysicalDpi {
    DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ~ProbePhysicalDpi() { if (previous) SetThreadDpiAwarenessContext(previous); }
};
BOOL CALLBACK FindProbeMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto& match = *reinterpret_cast<ProbeMonitor*>(parameter);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info)) &&
        match.name.compare(QString::fromWCharArray(info.szDevice), Qt::CaseInsensitive) == 0) {
        match.physical = info.rcMonitor;
        match.found = true;
        return FALSE;
    }
    return TRUE;
}
#endif
QJsonObject ProbeCoordinates(const MeetingUI::RemoteControlTarget& target) {
    QJsonObject result{{"coordinate_status", "BLOCKED"}, {"coordinate_error", "target_unavailable"},
                       {"physical_rect", QJsonValue::Null}};
    if (!target.widget || target.content.width() <= 1 || target.content.height() <= 1) return result;
    const auto origin = target.widget->mapToGlobal(QPoint(0, 0));
    const QRectF global(QPointF(origin) + target.content.topLeft(), target.content.size());
    if (!QRectF(target.widget->rect()).contains(target.content)) {
        result["coordinate_error"] = "target_outside_widget";
        return result;
    }
    const auto* screen = QGuiApplication::screenAt(global.center().toPoint());
    if (!screen || !QRectF(screen->geometry()).contains(global)) {
        result["coordinate_error"] = "target_spans_screens";
        return result;
    }
#ifdef _WIN32
    ProbeMonitor monitor{screen->name()};
    {
        // Keep the monitor query in the same physical coordinate space as the
        // native input owner, without changing the UI's persistent DPI context.
        const ProbePhysicalDpi physicalDpi;
        if (!physicalDpi.previous) {
            result["coordinate_error"] = "physical_dpi_context_unavailable";
            return result;
        }
        EnumDisplayMonitors(nullptr, nullptr, FindProbeMonitor, reinterpret_cast<LPARAM>(&monitor));
    }
    const auto dpr = screen->devicePixelRatio();
    const auto logical = screen->geometry();
    const auto nativeWidth = monitor.physical.right - monitor.physical.left;
    const auto nativeHeight = monitor.physical.bottom - monitor.physical.top;
    if (!monitor.found || !std::isfinite(dpr) || dpr <= 0 ||
        std::abs(target.widget->devicePixelRatioF() - dpr) > 0.01 ||
        std::abs(logical.width() * dpr - nativeWidth) > 2 ||
        std::abs(logical.height() * dpr - nativeHeight) > 2) {
        result["coordinate_error"] = "monitor_dpi_mapping_unverified";
        return result;
    }
    // Qt's global screen positions need not be scaled like their sizes. Anchor
    // each conversion at the matched native monitor origin, including negatives.
    const auto left = monitor.physical.left + std::lround((global.left() - logical.left()) * dpr);
    const auto top = monitor.physical.top + std::lround((global.top() - logical.top()) * dpr);
    const auto right = monitor.physical.left + std::lround((global.right() - logical.left()) * dpr);
    const auto bottom = monitor.physical.top + std::lround((global.bottom() - logical.top()) * dpr);
    if (right <= left || bottom <= top || left < monitor.physical.left || top < monitor.physical.top ||
        right > monitor.physical.right || bottom > monitor.physical.bottom) {
        result["coordinate_error"] = "physical_rect_outside_monitor";
        return result;
    }
    result["coordinate_status"] = "READY";
    result["coordinate_error"] = "none";
    result["physical_rect"] = QJsonObject{{"x", int(left)}, {"y", int(top)},
        {"width", int(right - left)}, {"height", int(bottom - top)}};
#else
    result["coordinate_error"] = "platform_unsupported";
#endif
    return result;
}
#endif
} // namespace
RemoteControlUi::RemoteControlUi(QWidget* stage, OpenMeeting::MeetingCoordinator* coordinator,
        std::function<std::vector<RemoteControlTarget>()> targets, std::function<void(bool)> annotation,
        std::function<void(const QString&)> hostStatus)
    : QObject(stage), stage_(stage), coordinator_(coordinator), targets_(std::move(targets)),
      blockAnnotation_(std::move(annotation)), hostStatus_(std::move(hostStatus)) {
    meetingActive_ = coordinator && coordinator->state() == OpenMeeting::MeetingState::InMeeting;
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
    configureProbe();
#endif
    entry_ = new QPushButton(tr("远程控制"),stage);
    // Match the paging overlay: stack above the native GPU surface without
    // promoting the stage/chrome, and follow moves of raster ancestors.
    new NativeChildGeometry(*entry_);
    entry_->setAttribute(Qt::WA_NativeWindow);
    entry_->setObjectName(QStringLiteral("remoteControlRequest"));
    AppTheme::setTone(*entry_,AppTheme::Tone::Dark);
    AppTheme::setStyleVariant(*entry_,"meeting-stage-action");
    connect(entry_,&QPushButton::clicked,this,[this] { showTargets(); });
    connect(coordinator,&OpenMeeting::MeetingCoordinator::remoteControlChanged,this,
        [this](rc::Projection p) { apply(std::move(p)); });
    connect(coordinator,&OpenMeeting::MeetingCoordinator::stateChanged,this,
        [this](OpenMeeting::MeetingState state,const QString&) {
            meetingActive_ = state == OpenMeeting::MeetingState::InMeeting;
            if (state != OpenMeeting::MeetingState::InMeeting) { stop(); apply({}); }
            else refreshTargets();
        });
    timer_ = new QTimer(this); timer_->setInterval(16);
    connect(timer_,&QTimer::timeout,this,[this] { tick(); }); timer_->start();
    qApp->installEventFilter(this); positionEntry();
}
RemoteControlUi::~RemoteControlUi() {
    qApp->removeEventFilter(this);
    if (frontendLease_) frontendLease_->valid = false;
    if (coordinator_) coordinator_->stopRemoteControl();
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
    probeWriter_.reset();
#endif
    delete consent_.data(); delete entry_;
}
void RemoteControlUi::positionEntry() {
    if (!stage_) return;
    entry_->adjustSize(); entry_->move(std::max(8,stage_->width()-entry_->width()-12),12);
    raiseEntry();
}
void RemoteControlUi::raiseEntry() {
    refreshTargets();
    if (!entry_->isHidden()) entry_->raise();
}
void RemoteControlUi::refreshTargets() {
    const bool host = projection_.state == rc::State::AwaitingReady || projection_.state == rc::State::Controlled;
    bool hasRemoteDesktop = false;
    if (coordinator_ && meetingActive_ && !host) {
        const auto targets = targets_();
        hasRemoteDesktop = std::any_of(targets.begin(), targets.end(), [](const auto& candidate) {
            return !candidate.identity.isEmpty() && !candidate.track.isEmpty();
        });
    }
    entry_->setVisible(hasRemoteDesktop);
}
std::optional<RemoteControlTarget> RemoteControlUi::target() const {
    for (auto t : targets_()) if (t.track.toStdString() == projection_.track &&
        t.identity.toStdString() == projection_.peer && t.widget && t.widget->isVisible() &&
        t.content.width() > 1 && t.content.height() > 1) return t;
    return {};
}
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
void RemoteControlUi::configureProbe() {
    try {
    // Match the existing product diagnostic opt-in. Environment variables alone
    // never enable a probe in an ordinary product session.
    if (!QCoreApplication::arguments().contains(QStringLiteral("--debug"))) return;
    const auto path = qEnvironmentVariable("LIVEKIT_UIA_REMOTE_CONTROL_PROBE");
    const auto run = qEnvironmentVariable("LIVEKIT_UIA_RUN_ID");
    if (path.isEmpty() || run.size() != 32 ||
        !std::all_of(run.begin(), run.end(), [](QChar c) {
            return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
                   (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
        })) return;
    // Syntactic checks only: even metadata queries can block on a UNC sink.
    if (!QDir::isAbsolutePath(path) || path.size() > 32'767) return;
    probeWriter_ = RemoteControlProbeWriter::Create([path](const QByteArray& bytes) {
        const QFileInfo destination(path);
        if (destination.isDir() || destination.isSymLink() ||
            !QDir(destination.absolutePath()).exists()) return;
        QSaveFile output(destination.absoluteFilePath());
        output.setDirectWriteFallback(false);
        if (output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size()) output.commit();
    });
    probeRunId_ = run;
    } catch (...) { }
}
void RemoteControlUi::captureProbe() {
    if (!probeWriter_) return;
    const auto now = rc::NowMs();
    if (probeLastWrite_ && now >= probeLastWrite_ && now - probeLastWrite_ < 100) return;
    probeLastWrite_ = now;
    try {
        const auto hash = [this](const std::string& value) {
            return value.empty() ? QString() : QString::fromLatin1(QCryptographicHash::hash(
                probeRunId_.toUtf8() + ':' + QByteArray::fromStdString(value), QCryptographicHash::Sha256).toHex());
        };
        std::optional<RemoteControlTarget> current;
        const auto allTargets = targets_();
        std::vector<RemoteControlTarget> targets;
        std::copy_if(allTargets.begin(), allTargets.end(), std::back_inserter(targets), SelectableTarget);
        QString binding = QStringLiteral("none");
        for (const auto& candidate : targets) {
            if (projection_.state != rc::State::Idle && candidate.identity.toStdString() == projection_.peer &&
                candidate.track.toStdString() == projection_.track && candidate.widget) {
                current = candidate;
                binding = QStringLiteral("active_projection");
                break;
            }
        }
        if (!current && projection_.state == rc::State::Idle && targets.size() == 1) {
            current = targets.front();
            binding = QStringLiteral("single_visible_target");
        }
        auto* window = stage_ ? stage_->window() : nullptr;
        auto frameTime = lastFrame_;
        if (current) {
            const auto frame = recentFrames_.find(current->track);
            frameTime = frame != recentFrames_.end() ? frame->second : 0;
        }
        QJsonObject viewport{{"available", current.has_value()},
            {"binding", binding},
            {"target_peer_hash", current ? hash(current->identity.toStdString()) : QString()},
            {"target_track_hash", current ? hash(current->track.toStdString()) : QString()},
            {"visible", current && current->widget->isVisible()},
            {"focused", current && current->widget->hasFocus()},
            {"foreground", window && QApplication::activeWindow() == window},
            {"minimized", window && window->isMinimized()},
            {"recent_frame", frameTime != 0 && now >= frameTime && now - frameTime <= 2000},
            {"frame_age_ms", frameTime && now >= frameTime ? QJsonValue(double(now - frameTime)) : QJsonValue(QJsonValue::Null)},
            {"coordinate_status", "BLOCKED"}, {"coordinate_error", "target_unavailable"},
            {"physical_rect", QJsonValue::Null}};
        if (current) {
            const auto coordinates = ProbeCoordinates(*current);
            for (auto i = coordinates.begin(); i != coordinates.end(); ++i) viewport[i.key()] = i.value();
        }
        QJsonObject payload{{"schema", 1}, {"scope", "P18_ORDINARY_DESKTOP_READ_ONLY"},
            {"run_id", probeRunId_}, {"pid", double(QCoreApplication::applicationPid())},
            {"state", ProbeState(projection_.state)}, {"reason", ProbeReason(projection_.reason)},
            {"input_paused", projection_.inputPaused}, {"controller_paused", projection_.controllerPaused},
            {"local_paused", localPaused_}, {"input_epoch", double(projection_.inputEpoch)},
            {"peer_hash", hash(projection_.peer)}, {"track_hash", hash(projection_.track)},
            {"share_width", projection_.width}, {"share_height", projection_.height},
            {"target_count", int(targets.size())}, {"viewport", viewport}};
        auto semanticPayload = payload;
        auto semanticViewport = viewport;
        semanticViewport.remove(QStringLiteral("frame_age_ms"));
        semanticPayload["viewport"] = semanticViewport;
        const auto semantic = QJsonDocument(semanticPayload).toJson(QJsonDocument::Compact);
        if (semantic != probeSemantic_) {
            ++probeRevision_;
            probeSemantic_ = semantic;
        }
        payload["revision"] = double(probeRevision_);
        payload["sequence"] = double(probeSequence_ + 1);
        payload["monotonic_ms"] = double(now);
        const auto bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        // Sequence numbers identify admitted observations. Latest-value
        // replacement or a failed sink can leave gaps, never a success claim.
        if (probeWriter_->TrySubmit(bytes))
            ++probeSequence_;
    } catch (...) {
        // The optional observer cannot change authorization, input, or session
        // transitions. Missing/stale output is rejected by its external reader.
    }
}
#endif
void RemoteControlUi::showTargets() {
    if (!coordinator_ || active()) { stop(); return; }
    refreshTargets();
    if (entry_->isHidden()) return;
    auto* menu = new QMenu(entry_); menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setObjectName(QStringLiteral("remoteControlTargets"));
    AppTheme::styleMenu(*menu,AppTheme::Tone::Dark);
    int targetIndex = 0;
    int unavailableIndex = 0;
    for (const auto& t : targets_()) {
        auto* action = menu->addAction(tr("申请控制 %1 的共享桌面").arg(t.name));
        const auto selectable = SelectableTarget(t);
        action->setObjectName(selectable ? QStringLiteral("remoteControlTarget%1").arg(targetIndex++)
            : QStringLiteral("remoteControlUnavailableTarget%1").arg(unavailableIndex++));
        action->setEnabled(selectable);
        connect(action,&QAction::triggered,this,[this,t] {
            if (!coordinator_) return;
            if (frontendLease_) frontendLease_->valid = false;
            frontendLease_ = std::make_shared<rc::Lease>();
            frontendLease_->current = [] { return true; };
            frontendLease_->deadline = rc::NowMs() + rc::LeaseMs;
            projection_.track = t.track.toStdString(); lastFrame_ = recentFrames_[t.track];
            coordinator_->requestRemoteControl(t.identity,t.track,frontendLease_);
        });
    }
    if (menu->isEmpty()) menu->addAction(tr("当前没有可见的远端屏幕共享"))->setEnabled(false);
    menu->popup(entry_->mapToGlobal(QPoint(0,entry_->height())));
}
void RemoteControlUi::activate() {
    if (!coordinator_ || activationPending_ || projection_.state != rc::State::AwaitingActivation) return;
    const auto t = target();
    const auto now = rc::NowMs();
    if (!t || !lastFrame_ || now-lastFrame_ > 2000 || stage_->window()->isMinimized() ||
        QApplication::activeWindow() != stage_->window()) {
        entry_->setToolTip(tr("对方已同意，返回会议并等待共享画面就绪后自动开始；点击可结束本次授权。"));
        return;
    }
    t->widget->setFocusPolicy(Qt::StrongFocus); t->widget->setFocus(Qt::OtherFocusReason);
    focusTarget_ = t->widget;
    if (t->widget->hasFocus()) {
        activationPending_ = true;
        coordinator_->activateRemoteControl(projection_.grant);
    }
}
void RemoteControlUi::clearInput() {
    motion_.reset(); pointerDown_ = false; heldKeys_.clear(); heldButtons_.clear();
}
void RemoteControlUi::pause(bool paused) {
    if (projection_.state != rc::State::Controlling || localPaused_ == paused) return;
    localPaused_ = paused; clearInput();
    if (coordinator_) coordinator_->pauseRemoteControl(paused,projection_.grant);
    entry_->setToolTip(paused ? tr("已暂停输入，返回共享画面后自动继续；授权仍有效。") : tr("正在等待对方恢复输入。"));
}
void RemoteControlUi::apply(rc::Projection p) {
    if (!p.request.empty() && p.request == cancelledRequest_ && p.state != rc::State::Idle) {
        if (coordinator_) coordinator_->stopRemoteControl();
        return;
    }
    const auto previous = projection_.state;
    if (p.state != rc::State::AwaitingActivation || p.grant != projection_.grant) activationPending_ = false;
    if (p.inputPaused || p.inputEpoch != projection_.inputEpoch || p.state != projection_.state) clearInput();
    projection_ = p;
    if (consent_ && (p.state != rc::State::AwaitingConsent ||
        consent_->property("requestId").toString().toStdString() != p.request)) {
        consent_->deleteLater(); consent_.clear();
    }
    const bool host = p.state == rc::State::AwaitingReady || p.state == rc::State::Controlled;
    blockAnnotation_(host);
    refreshTargets();
    if (!host && hostStatus_) hostStatus_({});
    entry_->setText(active() ? tr("结束远程控制") : tr("远程控制"));
    if (p.state == rc::State::Idle) {
        activationClick_ = false; localPaused_ = false; focusTarget_.clear();
        if (frontendLease_) frontendLease_->valid = false;
        frontendLease_.reset();
        entry_->setToolTip(p.reason == rc::Reason::None ? QString() : tr("控制已结束；如需继续，请重新申请授权。"));
        return;
    }
    QString name = QString::fromStdString(p.peer);
    if (coordinator_) for (const auto& participant : coordinator_->participants())
        if (participant.identity == name && !participant.name.isEmpty()) { name = participant.name; break; }
    if (p.state == rc::State::AwaitingConsent && !consent_) {
        auto* dialog = new QDialog(stage_->window(),Qt::Dialog | Qt::WindowStaysOnTopHint); consent_ = dialog;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setObjectName(QStringLiteral("remoteControlConsent"));
        dialog->setProperty("requestId",QString::fromStdString(p.request));
        dialog->setWindowTitle(tr("允许远程控制？"));
        AppTheme::setTone(*dialog,AppTheme::Tone::Dark);
        AppTheme::setStyleVariant(*dialog,"standard-dialog");
        auto* outer = new QVBoxLayout(dialog);
        auto* label = new QLabel(tr("%1 申请控制您正在共享的桌面。\n本地键鼠操作优先；全部松开并静止 1 秒后，对方可继续控制。\n点击共享条“终止控制”或按 Ctrl+Alt+F12 可结束授权。\n仅支持普通桌面，不支持管理员窗口或安全桌面。").arg(name),dialog);
        label->setTextFormat(Qt::PlainText); label->setWordWrap(true); outer->addWidget(label);
        auto* row = new QHBoxLayout; outer->addLayout(row);
        auto* deny = new QPushButton(tr("拒绝"),dialog); deny->setDefault(true); row->addWidget(deny);
        deny->setObjectName(QStringLiteral("remoteControlDeny"));
        auto* allow = new QPushButton(tr("允许本次控制"),dialog); allow->setAutoDefault(false);
        allow->setObjectName(QStringLiteral("remoteControlAllow")); row->addWidget(allow);
        connect(deny,&QPushButton::clicked,dialog,&QDialog::reject);
        connect(allow,&QPushButton::clicked,dialog,&QDialog::accept);
        connect(dialog,&QDialog::finished,this,[this,id = QString::fromStdString(p.request)](int result) {
            if (!coordinator_) return;
            if (result == QDialog::Accepted) blockAnnotation_(true);
            coordinator_->respondRemoteControl(id,result == QDialog::Accepted);
        });
        dialog->resize(480,180); dialog->show(); dialog->raise();
    }
    if (p.state == rc::State::AwaitingActivation) {
        entry_->setToolTip(tr("对方已同意，画面就绪后自动开始；点击可结束本次授权。"));
        activate();
    } else if (host) {
        if (hostStatus_) hostStatus_(p.state == rc::State::AwaitingReady
            ? tr("已允许 %1，等待对方开始（15 秒）").arg(name)
            : p.controllerPaused ? tr("%1 已暂停输入 · 等待对方返回共享画面").arg(name)
            : p.inputPaused ? tr("远控输入暂缓 · 本地操作结束后自动继续 · Ctrl+Alt+F12 退出")
            : tr("%1 正在控制桌面 · 本地操作优先 · Ctrl+Alt+F12 退出").arg(name));
    } else if (p.state == rc::State::Controlling) {
        entry_->setToolTip(localPaused_ ? tr("已暂停输入，返回共享画面后自动继续；授权仍有效。")
            : p.inputPaused ? tr("对端暂缓输入，准备就绪后自动继续。")
            : tr("正在控制 %1 · Ctrl+Alt+F12 退出").arg(name));
        if (previous != rc::State::Controlling) if (auto t = target()) {
            t->widget->setFocusPolicy(Qt::StrongFocus); t->widget->setFocus(Qt::OtherFocusReason);
            focusTarget_ = t->widget;
        }
    } else entry_->setToolTip(tr("等待对方确认，15 秒后自动取消"));
    positionEntry();
}
void RemoteControlUi::stop() {
    if (frontendLease_) frontendLease_->valid = false;
    frontendLease_.reset();
    cancelledRequest_ = projection_.request;
    clearInput(); activationClick_ = false; activationPending_ = false; localPaused_ = false; focusTarget_.clear();
    if (coordinator_) coordinator_->stopRemoteControl();
    // Local UI stops immediately; delayed projection cannot emit more inputs.
    projection_.state = rc::State::Idle;
    entry_->setText(tr("远程控制")); refreshTargets(); blockAnnotation_(false);
    if (consent_) { consent_->deleteLater(); consent_.clear(); }
    if (hostStatus_) hostStatus_({});
}
void RemoteControlUi::noteFrame(const QString& track) {
    recentFrames_[track] = rc::NowMs();
    if (recentFrames_.size() > 128) {
        const auto oldest = std::min_element(recentFrames_.begin(),recentFrames_.end(),
            [](const auto& a,const auto& b) { return a.second < b.second; });
        recentFrames_.erase(oldest);
    }
    if (track.toStdString() == projection_.track) lastFrame_ = rc::NowMs();
}
void RemoteControlUi::send(rc::Input i) {
    if (coordinator_ && projection_.state == rc::State::Controlling && !localPaused_ && !projection_.inputPaused)
        coordinator_->sendRemoteControlInput(i,projection_.grant,projection_.inputEpoch);
}
void RemoteControlUi::tick() {
    refreshTargets();
    updateSafety();
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
    captureProbe();
#endif
}
void RemoteControlUi::updateSafety() {
    const auto now = rc::NowMs();
    const bool controller = projection_.state == rc::State::Requesting ||
        projection_.state == rc::State::AwaitingActivation || projection_.state == rc::State::Controlling;
    if (frontendLease_ && controller && !frontendLease_->permits(now)) { stop(); return; }
    if (projection_.state == rc::State::Requesting || projection_.state == rc::State::AwaitingActivation) {
        if (frontendLease_) frontendLease_->deadline = now + rc::LeaseMs;
        if (projection_.state == rc::State::AwaitingActivation) activate();
        return;
    }
    if (projection_.state != rc::State::Controlling) return;
    // A live UI keeps the authorization lease while minimized/in another app.
    // Input admission, including release of held remote keys, is separate.
    if (frontendLease_) frontendLease_->deadline = now + rc::LeaseMs;
    const auto t = target();
    if (!t || !lastFrame_ || now-lastFrame_ > 2000 || QApplication::activeWindow() != stage_->window() ||
        stage_->window()->isMinimized() || !t->widget->hasFocus()) { pause(true); return; }
    if (projection_.width < 2 || projection_.height < 2 ||
        std::abs(t->content.width()/t->content.height()*projection_.height/projection_.width-1.0) > 0.02) {
        stop(); return;
    }
    pause(false);
    if (motion_ && now-lastMotion_ >= 33) { send(*motion_); motion_.reset(); lastMotion_ = now; }
}
bool RemoteControlUi::eventFilter(QObject* watched,QEvent* event) {
    if (watched == stage_ && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) positionEntry();
    if (projection_.state != rc::State::Controlling && projection_.state != rc::State::AwaitingActivation) return false;
    if (projection_.state == rc::State::Controlling &&
        ((watched == focusTarget_ && (event->type() == QEvent::FocusOut || event->type() == QEvent::Hide)) ||
         (watched == stage_->window() && (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide ||
          (event->type() == QEvent::WindowStateChange && stage_->window()->isMinimized()))))) {
        pause(true); return false;
    }
    const auto t = target();
    if (!t || watched != t->widget) return false;
    // The activation click is local UI intent, not an application click on
    // the remote desktop. Consume its matching release even if Ready arrived.
    if (activationClick_) {
        if (event->type() == QEvent::MouseButtonRelease) activationClick_ = false;
        if (event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseMove) return true;
    }
    if (projection_.state == rc::State::AwaitingActivation) {
        if (event->type() == QEvent::MouseButtonPress) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton && t->content.contains(mouse->localPos())) {
                activationClick_ = true; activate();
            }
            return true;
        }
        return false;
    }
    if (localPaused_ && event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton && t->content.contains(mouse->localPos())) {
            activationClick_ = true; t->widget->setFocus(Qt::MouseFocusReason); tick();
        }
        return true;
    }
    if (event->type() == QEvent::ShortcutOverride) { event->accept(); return true; }
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_F12 && key->modifiers().testFlag(Qt::ControlModifier) &&
            key->modifiers().testFlag(Qt::AltModifier)) { stop(); return true; }
        if (localPaused_ || projection_.inputPaused) return true;
        if (key->isAutoRepeat() && event->type() == QEvent::KeyRelease) return true;
        const auto scan = key->nativeScanCode();
        if ((key->isAutoRepeat() || event->type() == QEvent::KeyRelease) && !heldKeys_.contains(scan)) return true;
        rc::Input i; i.kind = rc::InputKind::Key; i.code = int(scan & 0xff);
        i.extended = (scan & 0xff00) != 0; i.down = event->type() == QEvent::KeyPress;
        motion_.reset(); if (i.valid()) {
            if (i.down) heldKeys_.insert(scan); else heldKeys_.erase(scan);
            send(i);
        }
        return true;
    }
    const bool mouse = event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick;
    if (!mouse && event->type() != QEvent::Wheel) return false;
    if (localPaused_ || projection_.inputPaused) return true;
    const auto point = mouse ? static_cast<QMouseEvent*>(event)->localPos() : static_cast<QWheelEvent*>(event)->position();
    const auto normalized = rc::NormalizePointer(point.x(),point.y(),t->content.left(),t->content.top(),
        t->content.width(),t->content.height());
    if (!normalized) { if (pointerDown_) pause(true); return true; }
    rc::Input i;
    i.x = uint16_t(normalized->x); i.y = uint16_t(normalized->y);
    if (event->type() == QEvent::Wheel) {
        i.kind = rc::InputKind::Wheel; i.code = std::clamp(static_cast<QWheelEvent*>(event)->angleDelta().y(),-1200,1200);
    } else if (event->type() != QEvent::MouseMove) {
        const auto* m = static_cast<QMouseEvent*>(event);
        i.kind = rc::InputKind::Button;
        i.code = m->button() == Qt::LeftButton ? 1 : m->button() == Qt::RightButton ? 2 : m->button() == Qt::MiddleButton ? 3 : 0;
        i.down = event->type() != QEvent::MouseButtonRelease;
        if (!i.down && !heldButtons_.contains(i.code)) return true;
        if (i.down) heldButtons_.insert(i.code); else heldButtons_.erase(i.code);
        pointerDown_ = !heldButtons_.empty();
    }
    if (event->type() == QEvent::MouseMove && !pointerDown_ && static_cast<QMouseEvent*>(event)->buttons() != Qt::NoButton) return true;
    if (!i.valid()) return true;
    if (i.kind == rc::InputKind::Move) motion_ = i;
    else { motion_.reset(); send(i); }
    return true;
}
} // namespace MeetingUI
