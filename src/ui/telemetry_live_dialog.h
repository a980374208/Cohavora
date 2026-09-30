#pragma once
#include "telemetry_panel_model.h"
class QDialog;
class QWidget;
namespace MeetingUI {
void CloseLiveTelemetryDialog(QDialog *dialog);
QDialog *OpenLiveTelemetryDialog(QWidget *parent,SessionKey key,
    std::weak_ptr<livekit::telemetry::TelemetryHistoryStore> store);
}
