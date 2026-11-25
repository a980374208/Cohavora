#include "src/ui/call/outgoing_call_overlay.h"

#include "src/ui/app_theme.h"
#include "src/ui/contacts/initials_avatar_painter.h"

#include <QtCore/QCoreApplication>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace MeetingUI {
namespace Theme = MeetingUI::AppTheme;

OutgoingCallOverlay::OutgoingCallOverlay(QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QCoreApplication::translate("MeetingUI", "Calling"));
    setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::CustomizeWindowHint);
    setModal(false);
    setFixedSize(320, 230);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 24);
    root->setSpacing(12);
    root->setAlignment(Qt::AlignCenter);

    _avatarLabel = new QLabel(this);
    _avatarLabel->setFixedSize(64, 64);
    _avatarLabel->setAlignment(Qt::AlignCenter);
    root->addWidget(_avatarLabel, 0, Qt::AlignHCenter);

    _nameLabel = new QLabel(this);
    QFont nf = _nameLabel->font();
    nf.setPixelSize(16);
    nf.setBold(true);
    _nameLabel->setFont(nf);
    _nameLabel->setAlignment(Qt::AlignCenter);
    root->addWidget(_nameLabel);

    _statusLabel = new QLabel(this);
    _statusLabel->setAlignment(Qt::AlignCenter);
    root->addWidget(_statusLabel);

    root->addSpacing(6);

    _cancelBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Cancel Call"), this);
    _cancelBtn->setCursor(Qt::PointingHandCursor);
    _cancelBtn->setFixedSize(140, 36);
    connect(_cancelBtn, &QPushButton::clicked, this, &OutgoingCallOverlay::cancelClicked);
    root->addWidget(_cancelBtn, 0, Qt::AlignHCenter);
}

void OutgoingCallOverlay::presentOutgoing(
        const OpenMeeting::CallSession &session, const QString &calleeDisplayName, int remainingSeconds) {
    const QString name = calleeDisplayName.trimmed().isEmpty()
        ? (session.calleeNickname.trimmed().isEmpty() ? session.calleeUserId : session.calleeNickname)
        : calleeDisplayName.trimmed();
    _nameLabel->setText(name);
    const QIcon icon = InitialsAvatarPainter::buildAvatarIcon(
        session.calleeUserId, name, 64, devicePixelRatioF(), OpenMeeting::PresenceStatus::Online);
    _avatarLabel->setPixmap(icon.pixmap(64, 64));
    updateCountdown(remainingSeconds);

    _cancelBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: #ffffff; border: none; border-radius: 8px; font-weight: 600; }")
        .arg(Theme::color(Theme::ColorRole::Danger).name()));
    AppTheme::setTone(*this, Theme::effectiveTheme() == Theme::Theme::Dark
        ? Theme::Tone::Dark : Theme::Tone::Light);
    show();
    raise();
}

void OutgoingCallOverlay::updateCountdown(int remainingSeconds) {
    _statusLabel->setText(QCoreApplication::translate("MeetingUI", "Waiting for answer... %1s")
        .arg(std::max(0, remainingSeconds)));
}

void OutgoingCallOverlay::dismiss() {
    hide();
}

void OutgoingCallOverlay::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        emit cancelClicked();
        return;
    }
    QDialog::keyPressEvent(event);
}

} // namespace MeetingUI
