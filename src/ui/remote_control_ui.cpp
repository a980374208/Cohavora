#include "remote_control_ui.h"
#include "src/core/meeting_coordinator.h"
#include "src/ui/app_theme.h"
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
RemoteControlUi::RemoteControlUi(QWidget* stage, OpenMeeting::MeetingCoordinator* coordinator,
        std::function<std::vector<RemoteControlTarget>()> targets, std::function<void(bool)> annotation,
        std::function<void(const QString&)> hostStatus)
    : QObject(stage), stage_(stage), coordinator_(coordinator), targets_(std::move(targets)),
      blockAnnotation_(std::move(annotation)), hostStatus_(std::move(hostStatus)) {
    entry_ = new QPushButton(tr("远程控制"),stage->window());
    entry_->setObjectName(QStringLiteral("remoteControlRequest"));
    AppTheme::setTone(*entry_,AppTheme::Tone::Dark);
    connect(entry_,&QPushButton::clicked,this,[this] { showTargets(); });
    connect(coordinator,&OpenMeeting::MeetingCoordinator::remoteControlChanged,this,
        [this](rc::Projection p) { apply(std::move(p)); });
    connect(coordinator,&OpenMeeting::MeetingCoordinator::stateChanged,this,
        [this](OpenMeeting::MeetingState state,const QString&) {
            if (state != OpenMeeting::MeetingState::InMeeting) { stop(); apply({}); }
        });
    timer_ = new QTimer(this); timer_->setInterval(16);
    connect(timer_,&QTimer::timeout,this,[this] { tick(); }); timer_->start();
    qApp->installEventFilter(this); positionEntry();
}
RemoteControlUi::~RemoteControlUi() {
    qApp->removeEventFilter(this);
    if (frontendLease_) frontendLease_->valid = false;
    if (coordinator_) coordinator_->stopRemoteControl();
    delete consent_.data(); delete entry_;
}
void RemoteControlUi::positionEntry() {
    if (!stage_) return;
    entry_->adjustSize(); entry_->move(stage_->mapTo(entry_->parentWidget(),
        QPoint(std::max(8,stage_->width()-entry_->width()-12),12)));
    entry_->raise();
}
std::optional<RemoteControlTarget> RemoteControlUi::target() const {
    for (auto t : targets_()) if (t.track.toStdString() == projection_.track &&
        t.identity.toStdString() == projection_.peer && t.widget && t.widget->isVisible() &&
        t.content.width() > 1 && t.content.height() > 1) return t;
    return {};
}
void RemoteControlUi::showTargets() {
    if (!coordinator_ || active()) { stop(); return; }
    auto* menu = new QMenu(entry_); menu->setAttribute(Qt::WA_DeleteOnClose);
    AppTheme::styleMenu(*menu,AppTheme::Tone::Dark);
    for (const auto& t : targets_()) {
        auto* action = menu->addAction(tr("申请控制 %1 的共享桌面").arg(t.name));
        action->setEnabled(t.widget && !t.content.isEmpty());
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
    entry_->setVisible(!host);
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
        auto* outer = new QVBoxLayout(dialog);
        auto* label = new QLabel(tr("%1 申请控制您正在共享的桌面。\n本地键鼠操作优先；全部松开并静止 1 秒后，对方可继续控制。\n点击共享条“终止控制”或按 Ctrl+Alt+F12 可结束授权。\n仅支持普通桌面，不支持管理员窗口或安全桌面。").arg(name),dialog);
        label->setTextFormat(Qt::PlainText); label->setWordWrap(true); outer->addWidget(label);
        auto* row = new QHBoxLayout; outer->addLayout(row);
        auto* deny = new QPushButton(tr("拒绝"),dialog); deny->setDefault(true); row->addWidget(deny);
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
    entry_->setText(tr("远程控制")); entry_->show(); blockAnnotation_(false);
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
