#pragma once

#include <QtWidgets/QWidget>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListView>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <memory>
#include <optional>

#include "src/core/meeting_coordinator.h"
#include "src/ui/participants_list_model.h"
#include "src/ui/participant_item_delegate.h"

namespace OpenMeeting {

class ParticipantsSidebarWidget : public QWidget {
    Q_OBJECT
public:
    explicit ParticipantsSidebarWidget(std::shared_ptr<MeetingCoordinator> coordinator, QWidget *parent = nullptr);
    ~ParticipantsSidebarWidget() override = default;

    void updateParticipants(const std::vector<ParticipantInfo> &participants);
    void setLocalMediaPreparation(std::optional<bool> audioMuted,
                                 std::optional<bool> videoEnabled,
                                 const QString &error = {});

signals:
    void closeRequested();
    void localMicrophoneRequested(bool muted);
    void localCameraRequested(bool enabled);

protected:
    void paintEvent(QPaintEvent *e) override;

private slots:
    void onSearchTextChanged(const QString &text);
    void onMicClicked(const QString &identity);
    void onCameraClicked(const QString &identity);
    void onMoreClicked(const QString &identity, const QPoint &globalPos);
    void onMuteAllClicked();
    void onUnmuteAllClicked();

private:
    void setupUi();
    void updateHostControlsVisibility();

    std::shared_ptr<MeetingCoordinator> _coordinator;

    QLabel *_titleLabel = nullptr;
    QPushButton *_closeBtn = nullptr;
    QLineEdit *_searchEdit = nullptr;
    QListView *_listView = nullptr;
    QWidget *_bottomPanel = nullptr;
    QPushButton *_muteAllBtn = nullptr;
    QPushButton *_unmuteAllBtn = nullptr;

    ParticipantListModel *_listModel = nullptr;
    ParticipantFilterProxyModel *_proxyModel = nullptr;
    ParticipantItemDelegate *_itemDelegate = nullptr;
    QLabel *_mediaStatusLabel = nullptr;
    std::optional<bool> _pendingAudioMuted;
    std::optional<bool> _pendingVideoEnabled;
};

} // namespace OpenMeeting
