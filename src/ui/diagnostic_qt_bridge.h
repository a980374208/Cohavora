#pragma once

#include "src/telemetry/diagnostic_pipeline.h"

#include <memory>

namespace MeetingUI {

class DiagnosticQtBridge final {
public:
	explicit DiagnosticQtBridge(std::shared_ptr<livekit::diagnostic::DiagnosticPipeline> pipeline);
	~DiagnosticQtBridge();
	DiagnosticQtBridge(const DiagnosticQtBridge&) = delete;
	DiagnosticQtBridge& operator=(const DiagnosticQtBridge&) = delete;

private:
	std::shared_ptr<livekit::diagnostic::DiagnosticPipeline> pipeline_;
};

} // namespace MeetingUI
