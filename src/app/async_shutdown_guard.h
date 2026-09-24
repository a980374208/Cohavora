#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QThread>
#include <QtCore/QTimer>

#include <functional>
#include <utility>

namespace MeetingApp {

// Keep the application dispatcher alive while native owners drain. Explicit
// Quit events and lastWindowClosed use the same idempotent transaction.
// In Qt 5, QCoreApplication::quit()/exit() bypass event filters. Application
// exit actions must call Request(); only the drained completion calls quit().
class AsyncShutdownGuard final : public QObject {
public:
    using Shutdown = std::function<void(std::function<void()>)>;
    using UnwindModal = std::function<void()>;

    AsyncShutdownGuard(QCoreApplication& application, Shutdown shutdown,
                       UnwindModal unwindModal = {})
        : QObject(&application), application_(application), shutdown_(std::move(shutdown)),
          unwind_modal_(std::move(unwindModal)) {
        application_.installEventFilter(this);
    }

    void Request() {
        if (started_) return;
        started_ = true;
        QMetaObject::invokeMethod(this, [this] { TryStart(); }, Qt::QueuedConnection);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == &application_ && event->type() == QEvent::Quit && !ready_) {
            Request();
            return true;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void TryStart() {
        // A queued call also runs inside QDialog::exec(). Its parent window
        // may still own stack-allocated dialogs, so wait for that stack to
        // unwind before destroying producers or closing cleanup admission.
        if (thread()->loopLevel() > 1) {
            const QPointer<AsyncShutdownGuard> guard(this);
            if (unwind_modal_) unwind_modal_();
            if (guard) {
                QTimer::singleShot(10, guard, [guard] {
                    if (guard) guard->TryStart();
                });
            }
            return;
        }
        const QPointer<AsyncShutdownGuard> guard(this);
        shutdown_([guard] {
            if (!guard) return;
            guard->ready_ = true;
            QCoreApplication::quit();
        });
    }

    QCoreApplication& application_;
    Shutdown shutdown_;
    UnwindModal unwind_modal_;
    bool started_ = false;
    bool ready_ = false;
};

} // namespace MeetingApp
