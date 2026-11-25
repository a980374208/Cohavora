#include "src/ui/call/incoming_call_window.h"

#include "src/ui/app_theme.h"
#include "src/ui/contacts/initials_avatar_painter.h"

#include <QtCore/QCoreApplication>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtGui/QScreen>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

namespace MeetingUI {
namespace Theme = MeetingUI::AppTheme;

IncomingCallWindow::IncomingCallWindow(QWidget *parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("IncomingCallWindow"));
    setWindowTitle(QCoreApplication::translate("MeetingUI", "Incoming Call"));
    const bool isWayland = QGuiApplication::platformName().contains(QStringLiteral("wayland"), Qt::CaseInsensitive);
    Qt::WindowFlags flags = Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint;
    if (!isWayland) {
        flags |= Qt::Tool | Qt::WindowDoesNotAcceptFocus;
    }
    setWindowFlags(flags);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, !isWayland);
    setFixedSize(360, 146);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(12);

    auto *topRow = new QHBoxLayout();
    topRow->setSpacing(12);
    _avatarLabel = new QLabel(this);
    _avatarLabel->setFixedSize(48, 48);
    topRow->addWidget(_avatarLabel);

    auto *infoCol = new QVBoxLayout();
    infoCol->setSpacing(3);
    _callerLabel = new QLabel(this);
    QFont nameFont = _callerLabel->font();
    nameFont.setPixelSize(15);
    nameFont.setBold(true);
    _callerLabel->setFont(nameFont);
    infoCol->addWidget(_callerLabel);

    _subtitleLabel = new QLabel(this);
    QFont subFont = _subtitleLabel->font();
    subFont.setPixelSize(12);
    _subtitleLabel->setFont(subFont);
    infoCol->addWidget(_subtitleLabel);

    _countdownLabel = new QLabel(this);
    QFont cdFont = _countdownLabel->font();
    cdFont.setPixelSize(11);
    _countdownLabel->setFont(cdFont);
    infoCol->addWidget(_countdownLabel);

    topRow->addLayout(infoCol, 1);
    root->addLayout(topRow);

    auto *btnRow = new QHBoxLayout();
    btnRow->setSpacing(10);
    _rejectBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Decline"), this);
    _rejectBtn->setCursor(Qt::PointingHandCursor);
    _rejectBtn->setFixedHeight(34);
    connect(_rejectBtn, &QPushButton::clicked, this, &IncomingCallWindow::rejectClicked);
    btnRow->addWidget(_rejectBtn, 1);

    _acceptBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Accept"), this);
    _acceptBtn->setCursor(Qt::PointingHandCursor);
    _acceptBtn->setFixedHeight(34);
    connect(_acceptBtn, &QPushButton::clicked, this, &IncomingCallWindow::acceptClicked);
    btnRow->addWidget(_acceptBtn, 1);
    root->addLayout(btnRow);
}

void IncomingCallWindow::presentCall(const OpenMeeting::CallSession &session, int remainingSeconds) {
    _session = session;
    const QString callerName = session.callerNickname.trimmed().isEmpty()
        ? session.callerUserId : session.callerNickname.trimmed();
    _callerLabel->setText(callerName);

    const QIcon avatarIcon = InitialsAvatarPainter::buildAvatarIcon(
        session.callerUserId, callerName, 48, devicePixelRatioF(), OpenMeeting::PresenceStatus::Online);
    _avatarLabel->setPixmap(avatarIcon.pixmap(48, 48));

    QString subtitle;
    if (session.mode == OpenMeeting::CallMode::MeetingInvite) {
        subtitle = session.meetingTitle.trimmed().isEmpty()
            ? QCoreApplication::translate("MeetingUI", "Inviting you to a meeting")
            : QCoreApplication::translate("MeetingUI", "Inviting you to: %1").arg(session.meetingTitle);
    } else if (session.mediaType == OpenMeeting::CallMediaType::Audio) {
        subtitle = QCoreApplication::translate("MeetingUI", "Incoming voice call");
    } else {
        subtitle = QCoreApplication::translate("MeetingUI", "Incoming video call");
    }
    if (session.e2eeRequired) {
        subtitle += QCoreApplication::translate("MeetingUI", " · E2EE");
    }
    _subtitleLabel->setText(subtitle);
    updateCountdown(remainingSeconds);

    const QColor textCol = Theme::color(Theme::ColorRole::Text);
    const QColor mutedCol = Theme::color(Theme::ColorRole::TextSecondary);
    _callerLabel->setStyleSheet(QStringLiteral("color: %1;").arg(textCol.name()));
    _subtitleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(mutedCol.name()));
    _countdownLabel->setStyleSheet(QStringLiteral("color: %1;").arg(mutedCol.name()));
    _acceptBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: #ffffff; border: none; border-radius: 7px; font-weight: 600; }"
        "QPushButton:hover { opacity: 0.92; }")
        .arg(Theme::color(Theme::ColorRole::Success).name()));
    _rejectBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: #ffffff; border: none; border-radius: 7px; font-weight: 600; }"
        "QPushButton:hover { opacity: 0.92; }")
        .arg(Theme::color(Theme::ColorRole::Danger).name()));

    positionAtScreenCorner();
    show();
    raise();
    QApplication::alert(this, 0);
}

void IncomingCallWindow::updateCountdown(int remainingSeconds) {
    _countdownLabel->setText(QCoreApplication::translate("MeetingUI", "Ringing... %1s").arg(std::max(0, remainingSeconds)));
}

void IncomingCallWindow::dismiss() {
    hide();
}

void IncomingCallWindow::positionAtScreenCorner() {
    if (QGuiApplication::platformName().contains(QStringLiteral("wayland"), Qt::CaseInsensitive)) {
        return;
    }
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect avail = screen->availableGeometry();
        move(avail.right() - width() - 20, avail.bottom() - height() - 20);
    }
}

void IncomingCallWindow::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
    p.setBrush(Theme::color(Theme::ColorRole::Surface));
    p.setPen(QPen(Theme::color(Theme::ColorRole::Border), 1.2));
    p.drawRoundedRect(r, 12, 12);
}

void IncomingCallWindow::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        emit rejectClicked();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace MeetingUI
