#pragma once
#include "src/core/remote_control/remote_control.h"
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QRectF>
#include <QtWidgets/QWidget>
#include <functional>
#include <map>
#include <set>
#include <vector>

class QPushButton;
class QLabel;
class QDialog;
class QTimer;
namespace OpenMeeting { class MeetingCoordinator; }
namespace MeetingUI {
struct RemoteControlTarget {
    QString identity, track, name;
    QPointer<QWidget> widget;
    QRectF content; // widget-local, logical pixels; actual displayed video
};
class RemoteControlUi final : public QObject {
public:
    RemoteControlUi(QWidget* stage, OpenMeeting::MeetingCoordinator*,
        std::function<std::vector<RemoteControlTarget>()> targets,
        std::function<void(bool)> blockAnnotation,
        std::function<void(const QString&)> hostStatus = {});
    ~RemoteControlUi() override;
    void noteFrame(const QString& track);
    void stop();
    bool active() const { return projection_.state != livekit::remote_control::State::Idle; }
protected:
    bool eventFilter(QObject*,QEvent*) override;
private:
    void showTargets();
    void activate();
    void pause(bool paused);
    void clearInput();
    void apply(livekit::remote_control::Projection);
    void tick();
    std::optional<RemoteControlTarget> target() const;
    void positionEntry();
    void send(livekit::remote_control::Input);
    QPointer<QWidget> stage_;
    QPointer<QWidget> focusTarget_;
    QPointer<OpenMeeting::MeetingCoordinator> coordinator_;
    std::function<std::vector<RemoteControlTarget>()> targets_;
    std::function<void(bool)> blockAnnotation_;
    std::function<void(const QString&)> hostStatus_;
    QPushButton* entry_ = nullptr;
    QPointer<QDialog> consent_;
    QTimer* timer_ = nullptr;
    livekit::remote_control::Projection projection_;
    std::shared_ptr<livekit::remote_control::Lease> frontendLease_;
    std::optional<livekit::remote_control::Input> motion_;
    uint64_t lastFrame_ = 0, lastMotion_ = 0;
    bool pointerDown_ = false;
    bool activationClick_ = false;
    bool activationPending_ = false;
    bool localPaused_ = false;
    std::set<unsigned> heldKeys_;
    std::set<int> heldButtons_;
    std::map<QString,uint64_t> recentFrames_;
    std::string cancelledRequest_;
};
}
