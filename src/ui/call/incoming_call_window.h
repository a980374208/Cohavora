#pragma once

#include "src/net/call_types.h"

#include <QtWidgets/QWidget>

class QLabel;
class QPushButton;

namespace MeetingUI {

// Global incoming call floating notification card.
// On Windows & X11 docks cleanly at the bottom-right corner without stealing keyboard focus.
// On Wayland gracefully degrades to a standard top-level dialog + taskbar alert.
class IncomingCallWindow final : public QWidget {
    Q_OBJECT
public:
    explicit IncomingCallWindow(QWidget *parent = nullptr);

    void presentCall(const OpenMeeting::CallSession &session, int remainingSeconds);
    void updateCountdown(int remainingSeconds);
    void dismiss();

signals:
    void acceptClicked();
    void rejectClicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void positionAtScreenCorner();

    OpenMeeting::CallSession _session;
    QLabel *_avatarLabel = nullptr;
    QLabel *_callerLabel = nullptr;
    QLabel *_subtitleLabel = nullptr;
    QLabel *_countdownLabel = nullptr;
    QPushButton *_acceptBtn = nullptr;
    QPushButton *_rejectBtn = nullptr;
};

} // namespace MeetingUI
