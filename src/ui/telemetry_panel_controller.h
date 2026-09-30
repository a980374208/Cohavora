#pragma once
#include "telemetry_panel_model.h"
#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <functional>

namespace MeetingUI {
class TelemetryPanelController final : public QObject {
    Q_OBJECT
public:
    using Reader = std::function<std::vector<livekit::telemetry::SafeTelemetryRecordPtr>()>;
    explicit TelemetryPanelController(std::weak_ptr<livekit::telemetry::TelemetryHistoryStore> store, QObject *parent=nullptr);
    explicit TelemetryPanelController(Reader reader, QObject *parent=nullptr);
    void BindSession(SessionKey key);
    void SetView(TelemetryPage page, int seconds);
    void SetPresentationVisible(bool visible);
    void SetKeepCollecting(bool enabled);
    void Stop();
    bool IsPolling() const { return timer_.isActive(); }
signals:
    void FrameReady(MeetingUI::PanelFramePtr frame);
private:
    void Refresh();
    void Publish(bool forceMemory=false);
    PanelFramePtr previous_;
    TelemetryClock::time_point memoryPublishedAt_{};
    Reader reader_;
    SessionKey key_;
    TelemetryPanelModel model_;
    QTimer timer_;
    TelemetryPage page_ = TelemetryPage::Network;
    int seconds_ = 0;
    bool visible_ = false, stopped_ = false, keepCollecting_ = false;
};
} // namespace MeetingUI
