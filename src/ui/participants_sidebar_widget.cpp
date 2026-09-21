#include <QtCore/QCoreApplication>
#include "src/ui/participants_sidebar_widget.h"
#include "src/ui/app_theme.h"
#include "src/net/session_manager.h"

#include <QtCore/QDebug>
#include <QtWidgets/QAction>
#include <QtGui/QPainter>

namespace OpenMeeting {

ParticipantsSidebarWidget::ParticipantsSidebarWidget(std::shared_ptr<MeetingCoordinator> coordinator, QWidget *parent)
    : QWidget(parent)
    , _coordinator(coordinator) {
    MeetingUI::AppTheme::setTone(*this, MeetingUI::AppTheme::Tone::Dark);
    setupUi();

    if (_coordinator) {
        connect(_coordinator.get(), &MeetingCoordinator::participantsUpdated,
                this, &ParticipantsSidebarWidget::updateParticipants);
        updateParticipants(_coordinator->participants());
    }
}

void ParticipantsSidebarWidget::setupUi() {
    setMinimumWidth(280);
    MeetingUI::AppTheme::setStyleVariant(*this, "participants-sidebar-widget-this");
    setObjectName("ParticipantsSidebar");

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(2, 0, 0, 0);
    mainLayout->setSpacing(0);

    // 1. Header
    auto *headerWidget = new QWidget(this);
    headerWidget->setMinimumHeight(44);
    auto *headerLayout = new QHBoxLayout(headerWidget);
    headerLayout->setContentsMargins(16, 0, 12, 0);

    _titleLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Participants (0)"), headerWidget);
    _titleLabel->setObjectName("SidebarTitle");
    _titleLabel->setWordWrap(true);

    _closeBtn = new QPushButton(QString::fromUtf8("✕"), headerWidget);
    _closeBtn->setObjectName("CloseBtn");
    _closeBtn->setCursor(Qt::PointingHandCursor);
    connect(_closeBtn, &QPushButton::clicked, this, &ParticipantsSidebarWidget::closeRequested);

    headerLayout->addWidget(_titleLabel);
    headerLayout->addStretch();
    headerLayout->addWidget(_closeBtn);
    mainLayout->addWidget(headerWidget);

    // 2. 搜索框
    auto *searchContainer = new QWidget(this);
    auto *searchLayout = new QHBoxLayout(searchContainer);
    searchLayout->setContentsMargins(12, 4, 12, 8);

    _searchEdit = new QLineEdit(searchContainer);
    _searchEdit->setObjectName("SearchEdit");
    _searchEdit->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Search participants..."));
    _searchEdit->setClearButtonEnabled(true);
    connect(_searchEdit, &QLineEdit::textChanged, this, &ParticipantsSidebarWidget::onSearchTextChanged);
    searchLayout->addWidget(_searchEdit);
    mainLayout->addWidget(searchContainer);

    // 3. 参会人列表 (QListView + Model/Delegate)
    _listModel = new ParticipantListModel(this);
    _proxyModel = new ParticipantFilterProxyModel(this);
    _proxyModel->setSourceModel(_listModel);

    _listView = new QListView(this);
    _listView->setObjectName("ParticipantList");
    _listView->setModel(_proxyModel);
    _listView->setUniformItemSizes(true);
    _listView->setLayoutMode(QListView::Batched);
    _listView->setBatchSize(100);
    _listView->setSelectionMode(QAbstractItemView::NoSelection);
    _listView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    _itemDelegate = new ParticipantItemDelegate(this);
    _listView->setItemDelegate(_itemDelegate);

    connect(_itemDelegate, &ParticipantItemDelegate::micClicked, this, &ParticipantsSidebarWidget::onMicClicked);
    connect(_itemDelegate, &ParticipantItemDelegate::cameraClicked, this, &ParticipantsSidebarWidget::onCameraClicked);
    connect(_itemDelegate, &ParticipantItemDelegate::moreClicked, this, &ParticipantsSidebarWidget::onMoreClicked);

    mainLayout->addWidget(_listView, 1);

    // 4. 底部全员会控面板
    _bottomPanel = new QWidget(this);
    _bottomPanel->setObjectName("BottomPanel");
    _bottomPanel->setMinimumHeight(52);
    auto *bottomLayout = new QHBoxLayout(_bottomPanel);
    bottomLayout->setContentsMargins(12, 0, 12, 0);
    bottomLayout->setSpacing(10);

    _muteAllBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Mute All"), _bottomPanel);
    _muteAllBtn->setObjectName("MuteAllBtn");
    _muteAllBtn->setCursor(Qt::PointingHandCursor);
    connect(_muteAllBtn, &QPushButton::clicked, this, &ParticipantsSidebarWidget::onMuteAllClicked);

    _unmuteAllBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Unmute"), _bottomPanel);
    _unmuteAllBtn->setObjectName("UnmuteAllBtn");
    _unmuteAllBtn->setCursor(Qt::PointingHandCursor);
    connect(_unmuteAllBtn, &QPushButton::clicked, this, &ParticipantsSidebarWidget::onUnmuteAllClicked);

    bottomLayout->addWidget(_muteAllBtn, 1);
    bottomLayout->addWidget(_unmuteAllBtn, 1);
    mainLayout->addWidget(_bottomPanel);

    updateHostControlsVisibility();
}

void ParticipantsSidebarWidget::updateParticipants(const std::vector<ParticipantInfo> &participants) {
    _listModel->setParticipants(participants);
    QString mId = _coordinator ? _coordinator->currentMeetingId() : "";
    if (!mId.isEmpty()) {
        _titleLabel->setText(QCoreApplication::translate("MeetingUI", "Participants (%1) · Meeting ID: %2").arg(participants.size()).arg(mId));
    } else {
        _titleLabel->setText(QCoreApplication::translate("MeetingUI", "Participants (%1)").arg(participants.size()));
    }
    updateHostControlsVisibility();
}

void ParticipantsSidebarWidget::updateHostControlsVisibility() {
    bool isHostUser = _coordinator ? _coordinator->isHost() : false;
    _bottomPanel->setVisible(isHostUser);
}

void ParticipantsSidebarWidget::onSearchTextChanged(const QString &text) {
    _proxyModel->setSearchKeyword(text);
}

void ParticipantsSidebarWidget::onMicClicked(const QString &identity) {
    if (!_coordinator) return;

    ParticipantInfo target = _listModel->findParticipantById(identity);
    if (target.identity.isEmpty()) return;

    if (target.isLocal) {
        _coordinator->setLocalAudioMuted(!target.isAudioMuted);
    } else if (_coordinator->isHost()) {
        // 主持人控制他人
        bool newMute = !target.isAudioMuted;
        _coordinator->requestParticipantMicrophone(target.identity, !newMute);
    }
}

void ParticipantsSidebarWidget::onCameraClicked(const QString &identity) {
    if (!_coordinator) return;

    ParticipantInfo target = _listModel->findParticipantById(identity);
    if (target.identity.isEmpty()) return;

    if (target.isLocal) {
        _coordinator->setLocalVideoEnabled(!target.isVideoEnabled);
    } else if (_coordinator->isHost()) {
        // 主持人控制他人摄像头
        bool newEnable = !target.isVideoEnabled;
        _coordinator->requestParticipantCamera(target.identity, newEnable);
    }
}

void ParticipantsSidebarWidget::onMoreClicked(const QString &identity, const QPoint &globalPos) {
    if (!_coordinator) return;

    ParticipantInfo target = _listModel->findParticipantById(identity);
    if (target.identity.isEmpty()) return;

    QMenu menu(this);
    MeetingUI::AppTheme::styleMenu(menu, MeetingUI::AppTheme::Tone::Dark);

    if (target.isLocal) {
        // 本地用户菜单
        auto *infoAct = menu.addAction(QCoreApplication::translate("MeetingUI", "Role: Local Participant"));
        infoAct->setEnabled(false);
        auto *toggleMicAct = menu.addAction(target.isAudioMuted ? QCoreApplication::translate("MeetingUI", "Enable Microphone") : QCoreApplication::translate("MeetingUI", "Mute Myself"));
        connect(toggleMicAct, &QAction::triggered, this, [this, target]() {
            _coordinator->setLocalAudioMuted(!target.isAudioMuted);
        });
        auto *toggleCamAct = menu.addAction(target.isVideoEnabled ? QCoreApplication::translate("MeetingUI", "Turn Off Camera") : QCoreApplication::translate("MeetingUI", "Turn On Camera"));
        connect(toggleCamAct, &QAction::triggered, this, [this, target]() {
            _coordinator->setLocalVideoEnabled(!target.isVideoEnabled);
        });
    } else {
        // 远端参会人
        bool canAdmin = _coordinator->isHost();
        if (canAdmin) {
            auto *toggleMic = menu.addAction(target.isAudioMuted ? QCoreApplication::translate("MeetingUI", "Request Unmute") : QCoreApplication::translate("MeetingUI", "Mute Participant"));
            connect(toggleMic, &QAction::triggered, this, [this, target]() {
                _coordinator->requestParticipantMicrophone(target.identity, target.isAudioMuted);
            });

            auto *toggleCam = menu.addAction(target.isVideoEnabled ? QCoreApplication::translate("MeetingUI", "Turn Off Participant Video") : QCoreApplication::translate("MeetingUI", "Request Camera On"));
            connect(toggleCam, &QAction::triggered, this, [this, target]() {
                _coordinator->requestParticipantCamera(target.identity, !target.isVideoEnabled);
            });

            menu.addSeparator();

            auto *transferAct = menu.addAction(QCoreApplication::translate("MeetingUI", "Transfer Host"));
            connect(transferAct, &QAction::triggered, this, [this, target]() {
                auto ret = QMessageBox::question(this, QCoreApplication::translate("MeetingUI", "Transfer Host"),
                    QCoreApplication::translate("MeetingUI", "Transfer host permissions to \"%1\"?").arg(target.name));
                if (ret == QMessageBox::Yes) {
                    _coordinator->transferHost(target.identity);
                }
            });

            auto *kickAct = menu.addAction(QCoreApplication::translate("MeetingUI", "Remove from Meeting"));
            connect(kickAct, &QAction::triggered, this, [this, target]() {
                auto ret = QMessageBox::warning(this, QCoreApplication::translate("MeetingUI", "Confirm Removal"),
                    QCoreApplication::translate("MeetingUI", "Remove \"%1\" from this meeting?").arg(target.name),
                    QMessageBox::Yes | QMessageBox::Cancel);
                if (ret == QMessageBox::Yes) {
                    _coordinator->kickParticipant(target.identity, QCoreApplication::translate("MeetingUI", "Removed from the meeting by the host"));
                }
            });
        } else {
            auto *infoAct = menu.addAction(QCoreApplication::translate("MeetingUI", "Participant: %1").arg(target.name));
            infoAct->setEnabled(false);
        }
    }

    menu.exec(globalPos);
}

void ParticipantsSidebarWidget::onMuteAllClicked() {
    if (!_coordinator || !_coordinator->isHost()) return;
    auto ret = QMessageBox::question(this, QCoreApplication::translate("MeetingUI", "Mute All"),
        QCoreApplication::translate("MeetingUI", "Mute all participants?"),
        QMessageBox::Yes | QMessageBox::No);
    if (ret == QMessageBox::Yes) {
        _coordinator->muteAllParticipants(true);
    }
}

void ParticipantsSidebarWidget::onUnmuteAllClicked() {
    if (!_coordinator || !_coordinator->isHost()) return;
    _coordinator->muteAllParticipants(false);
}

void ParticipantsSidebarWidget::paintEvent(QPaintEvent *e) {
    Q_UNUSED(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    // 1. 填充主体背景色
    p.fillRect(rect(), QColor("#1A1D24"));

    // 2. 明显又不突兀的立体双层分界线
    // 舞台侧深邃暗阴影基底线 (x=0)
    p.setPen(QPen(QColor(8, 10, 14), 1));
    p.drawLine(0, 0, 0, height());

    // 侧边栏侧中度冷灰高光分割线 (x=1) - 清晰界定面板边缘
    p.setPen(QPen(QColor(58, 66, 82), 1));
    p.drawLine(1, 0, 1, height());

    // 3. 细微边缘层级环境阴影 (x=2 ~ 10)
    QLinearGradient shadow(2, 0, 10, 0);
    shadow.setColorAt(0.0, QColor(0, 0, 0, 45));
    shadow.setColorAt(1.0, QColor(0, 0, 0, 0));
    p.fillRect(QRect(2, 0, 8, height()), shadow);
}

} // namespace OpenMeeting
