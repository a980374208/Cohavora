#include "src/ui/participants_sidebar_widget.h"
#include "src/net/session_manager.h"

#include <QtCore/QDebug>
#include <QtWidgets/QAction>

namespace OpenMeeting {

ParticipantsSidebarWidget::ParticipantsSidebarWidget(std::shared_ptr<MeetingCoordinator> coordinator, QWidget *parent)
    : QWidget(parent)
    , _coordinator(coordinator) {
    setupUi();

    if (_coordinator) {
        connect(_coordinator.get(), &MeetingCoordinator::participantsUpdated,
                this, &ParticipantsSidebarWidget::updateParticipants);
        updateParticipants(_coordinator->participants());
    }
}

void ParticipantsSidebarWidget::setupUi() {
    setFixedWidth(300);
    setStyleSheet(
        "QWidget#ParticipantsSidebar {"
        "  background-color: #1A1D24;"
        "  border-left: 1px solid #2B303C;"
        "}"
        "QLabel#SidebarTitle {"
        "  color: #F3F4F6;"
        "  font-size: 14px;"
        "  font-weight: bold;"
        "}"
        "QPushButton#CloseBtn {"
        "  background: transparent;"
        "  color: #9CA3AF;"
        "  font-size: 16px;"
        "  border: none;"
        "  border-radius: 4px;"
        "  min-width: 24px;"
        "  max-width: 24px;"
        "  min-height: 24px;"
        "  max-height: 24px;"
        "}"
        "QPushButton#CloseBtn:hover {"
        "  background-color: rgba(255, 255, 255, 0.1);"
        "  color: #FFFFFF;"
        "}"
        "QLineEdit#SearchEdit {"
        "  background-color: #242831;"
        "  border: 1px solid #363C4A;"
        "  border-radius: 4px;"
        "  color: #F3F4F6;"
        "  padding: 4px 8px;"
        "  font-size: 12px;"
        "}"
        "QLineEdit#SearchEdit:focus {"
        "  border: 1px solid #3B82F6;"
        "}"
        "QListView#ParticipantList {"
        "  background-color: transparent;"
        "  border: none;"
        "  outline: none;"
        "}"
        "QListView#ParticipantList::item {"
        "  border-bottom: 1px solid #232731;"
        "}"
        "QScrollBar:vertical {"
        "  border: none;"
        "  background: transparent;"
        "  width: 6px;"
        "  margin: 0px 0px 0px 0px;"
        "}"
        "QScrollBar::handle:vertical {"
        "  background: #4B5563;"
        "  min-height: 20px;"
        "  border-radius: 3px;"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        "  height: 0px;"
        "}"
        "QWidget#BottomPanel {"
        "  background-color: #171A20;"
        "  border-top: 1px solid #2B303C;"
        "}"
        "QPushButton#MuteAllBtn {"
        "  background-color: #EF4444;"
        "  color: #FFFFFF;"
        "  border: none;"
        "  border-radius: 4px;"
        "  padding: 6px 12px;"
        "  font-size: 12px;"
        "  font-weight: 500;"
        "}"
        "QPushButton#MuteAllBtn:hover {"
        "  background-color: #DC2626;"
        "}"
        "QPushButton#UnmuteAllBtn {"
        "  background-color: #374151;"
        "  color: #F3F4F6;"
        "  border: none;"
        "  border-radius: 4px;"
        "  padding: 6px 12px;"
        "  font-size: 12px;"
        "}"
        "QPushButton#UnmuteAllBtn:hover {"
        "  background-color: #4B5563;"
        "}"
    );
    setObjectName("ParticipantsSidebar");

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // 1. Header
    auto *headerWidget = new QWidget(this);
    headerWidget->setFixedHeight(44);
    auto *headerLayout = new QHBoxLayout(headerWidget);
    headerLayout->setContentsMargins(16, 0, 12, 0);

    _titleLabel = new QLabel(QString::fromUtf8("参会人 (0)"), headerWidget);
    _titleLabel->setObjectName("SidebarTitle");

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
    _searchEdit->setPlaceholderText(QString::fromUtf8("搜索参会人..."));
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
    _bottomPanel->setFixedHeight(52);
    auto *bottomLayout = new QHBoxLayout(_bottomPanel);
    bottomLayout->setContentsMargins(12, 0, 12, 0);
    bottomLayout->setSpacing(10);

    _muteAllBtn = new QPushButton(QString::fromUtf8("全员静音"), _bottomPanel);
    _muteAllBtn->setObjectName("MuteAllBtn");
    _muteAllBtn->setCursor(Qt::PointingHandCursor);
    connect(_muteAllBtn, &QPushButton::clicked, this, &ParticipantsSidebarWidget::onMuteAllClicked);

    _unmuteAllBtn = new QPushButton(QString::fromUtf8("解除静音"), _bottomPanel);
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
        _titleLabel->setText(QString::fromUtf8("参会人 (%1) · 会议号: %2").arg(participants.size()).arg(mId));
    } else {
        _titleLabel->setText(QString::fromUtf8("参会人 (%1)").arg(participants.size()));
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
    menu.setStyleSheet(
        "QMenu {"
        "  background-color: #22262E;"
        "  border: 1px solid #363C4A;"
        "  border-radius: 6px;"
        "  padding: 4px;"
        "}"
        "QMenu::item {"
        "  color: #E5E7EB;"
        "  padding: 6px 20px;"
        "  border-radius: 4px;"
        "  font-size: 12px;"
        "}"
        "QMenu::item:selected {"
        "  background-color: #3B82F6;"
        "  color: #FFFFFF;"
        "}"
        "QMenu::separator {"
        "  height: 1px;"
        "  background-color: #363C4A;"
        "  margin: 4px 6px;"
        "}"
    );

    if (target.isLocal) {
        // 本地用户菜单
        auto *infoAct = menu.addAction(QString::fromUtf8("身份: 本机参与者"));
        infoAct->setEnabled(false);
        auto *toggleMicAct = menu.addAction(target.isAudioMuted ? QString::fromUtf8("开启麦克风") : QString::fromUtf8("静音自己"));
        connect(toggleMicAct, &QAction::triggered, this, [this, target]() {
            _coordinator->setLocalAudioMuted(!target.isAudioMuted);
        });
        auto *toggleCamAct = menu.addAction(target.isVideoEnabled ? QString::fromUtf8("关闭摄像头") : QString::fromUtf8("开启摄像头"));
        connect(toggleCamAct, &QAction::triggered, this, [this, target]() {
            _coordinator->setLocalVideoEnabled(!target.isVideoEnabled);
        });
    } else {
        // 远端参会人
        bool canAdmin = _coordinator->isHost();
        if (canAdmin) {
            auto *toggleMic = menu.addAction(target.isAudioMuted ? QString::fromUtf8("请求开启麦克风") : QString::fromUtf8("静音该成员"));
            connect(toggleMic, &QAction::triggered, this, [this, target]() {
                _coordinator->requestParticipantMicrophone(target.identity, target.isAudioMuted);
            });

            auto *toggleCam = menu.addAction(target.isVideoEnabled ? QString::fromUtf8("关闭该成员视频") : QString::fromUtf8("请求开启摄像头"));
            connect(toggleCam, &QAction::triggered, this, [this, target]() {
                _coordinator->requestParticipantCamera(target.identity, !target.isVideoEnabled);
            });

            menu.addSeparator();

            auto *transferAct = menu.addAction(QString::fromUtf8("移交主持人"));
            connect(transferAct, &QAction::triggered, this, [this, target]() {
                auto ret = QMessageBox::question(this, QString::fromUtf8("移交主持人"),
                    QString::fromUtf8("确定要将主持人权限移交给【%1】吗？").arg(target.name));
                if (ret == QMessageBox::Yes) {
                    _coordinator->transferHost(target.identity);
                }
            });

            auto *kickAct = menu.addAction(QString::fromUtf8("移出会议"));
            connect(kickAct, &QAction::triggered, this, [this, target]() {
                auto ret = QMessageBox::warning(this, QString::fromUtf8("移出会议确认"),
                    QString::fromUtf8("确定要将【%1】移出当前会议吗？").arg(target.name),
                    QMessageBox::Yes | QMessageBox::Cancel);
                if (ret == QMessageBox::Yes) {
                    _coordinator->kickParticipant(target.identity, QString::fromUtf8("已被主持人移出会议"));
                }
            });
        } else {
            auto *infoAct = menu.addAction(QString::fromUtf8("参会人: %1").arg(target.name));
            infoAct->setEnabled(false);
        }
    }

    menu.exec(globalPos);
}

void ParticipantsSidebarWidget::onMuteAllClicked() {
    if (!_coordinator || !_coordinator->isHost()) return;
    auto ret = QMessageBox::question(this, QString::fromUtf8("全员静音"),
        QString::fromUtf8("确定要静音所有参会成员吗？"),
        QMessageBox::Yes | QMessageBox::No);
    if (ret == QMessageBox::Yes) {
        _coordinator->muteAllParticipants(true);
    }
}

void ParticipantsSidebarWidget::onUnmuteAllClicked() {
    if (!_coordinator || !_coordinator->isHost()) return;
    _coordinator->muteAllParticipants(false);
}

} // namespace OpenMeeting
