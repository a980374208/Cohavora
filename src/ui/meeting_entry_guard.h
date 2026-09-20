#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QThread>
#include <memory>

namespace MeetingUI {

// Shared by all meeting entry points in the application's main window.
// Keep the reservation through dialogs and admission, then parent it to the
// room window. QObject deletes it after the window's media/Coordinator cleanup.
class MeetingEntryGuard final {
public:
    std::unique_ptr<QObject> tryAcquire() {
        Q_ASSERT(QCoreApplication::instance());
        Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
        if (_owner) return {};
        auto owner = std::make_unique<QObject>();
        _owner = owner.get();
        return owner;
    }

private:
    QPointer<QObject> _owner;
};

} // namespace MeetingUI
