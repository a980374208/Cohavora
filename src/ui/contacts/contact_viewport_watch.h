#pragma once

#include "src/core/presence_watch_controller.h"

#include <QtCore/QPointer>
#include <QtWidgets/QAbstractItemView>

#include <functional>

namespace MeetingUI {

// One source owns all mutually exclusive views on a page or in a modal picker.
class ContactViewportWatch final : public QObject {
public:
    ContactViewportWatch(QWidget &owner, OpenMeeting::PresenceWatchController &presence,
                         OpenMeeting::PresenceWatchController::Viewport kind,
                         std::function<QString()> detailUser = {});
    ~ContactViewportWatch() override;

    void observe(QAbstractItemView &view);
    void schedule();

private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void publish();

    QWidget &owner_;
    QPointer<OpenMeeting::PresenceWatchController> presence_;
    OpenMeeting::PresenceWatchController::Viewport kind_;
    std::function<QString()> detailUser_;
    QVector<QPointer<QAbstractItemView>> views_;
    QTimer timer_;
};

} // namespace MeetingUI
