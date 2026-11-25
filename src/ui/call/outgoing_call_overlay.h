#pragma once

#include "src/net/call_types.h"

#include <QtWidgets/QDialog>

class QLabel;
class QPushButton;

namespace MeetingUI {

class OutgoingCallOverlay final : public QDialog {
    Q_OBJECT
public:
    explicit OutgoingCallOverlay(QWidget *parent = nullptr);

    void presentOutgoing(const OpenMeeting::CallSession &session, const QString &calleeDisplayName, int remainingSeconds);
    void updateCountdown(int remainingSeconds);
    void dismiss();

signals:
    void cancelClicked();

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    QLabel *_avatarLabel = nullptr;
    QLabel *_nameLabel = nullptr;
    QLabel *_statusLabel = nullptr;
    QPushButton *_cancelBtn = nullptr;
};

} // namespace MeetingUI
