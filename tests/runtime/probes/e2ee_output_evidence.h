#pragma once

#include "src/telemetry/diagnostic_pipeline.h"
#include "src/telemetry/diagnostic_spdlog_bridge.h"
#include "src/telemetry/telemetry_report.h"
#include "src/ui/diagnostic_qt_bridge.h"
#include <QtCore/QString>
#include <QtCore/QJsonObject>
#include <future>

// Opt-in real product persistence/export paths, rooted inside this run only.
class E2eeOutputEvidence final {
public:
    explicit E2eeOutputEvidence(const QString& directory)
        : enabled_(qEnvironmentVariable("E2EE_OUTPUT_EVIDENCE") == "1"), root_(directory.toStdWString()) {
        if (!enabled_) return;
        pipeline_ = std::make_shared<livekit::diagnostic::DiagnosticPipeline>();
        livekit::diagnostic::InstallBusinessPipeline(pipeline_);
        ready_ = pipeline_->StartWriter(root_ / "diagnostics") &&
            livekit::diagnostic::InstallSafeSpdlogAdapter(pipeline_);
        qt_ = std::make_unique<MeetingUI::DiagnosticQtBridge>(pipeline_);
        history_ = std::make_shared<livekit::telemetry::TelemetryHistoryStore>(
            root_ / "history", std::string(pipeline_->run_id()), root_ / "diagnostics");
        livekit::telemetry::InstallTelemetryHistoryStore(history_);
    }
    ~E2eeOutputEvidence() {
        if (!enabled_) return;
        qt_.reset();
        livekit::telemetry::InstallTelemetryHistoryStore({});
        livekit::diagnostic::InstallBusinessPipeline({});
        if (history_) history_->Close();
        if (pipeline_) pipeline_->Close();
    }
    bool ready() const { return ready_; }
    bool enabled() const { return enabled_; }
    QJsonObject finish() {
        const bool logs = pipeline_->Close() == livekit::diagnostic::DrainResult::Completed;
        const auto exportOne = [&](bool bundle) {
            auto promise = std::make_shared<std::promise<bool>>();
            auto future = promise->get_future();
            const auto callback = [promise](livekit::telemetry::TelemetryExportResult result) { promise->set_value(result.success); };
            if (bundle) {
                const auto status = history_->Status();
                if (status->reports.empty()) return false;
                history_->ExportReport(status->reports.back().record_id, root_ / "bundle-export", callback);
            } else history_->ExportCurrent(root_ / "current-export", callback);
            return future.wait_for(std::chrono::seconds(10)) == std::future_status::ready && future.get();
        };
        const bool current = exportOne(false);
        const bool bundle = exportOne(true);
        const bool history = history_->Close().state == livekit::telemetry::TelemetryCloseState::Completed;
        return {{"diagnostics_drained", logs}, {"current_export", current},
            {"bundle_export", bundle}, {"history_drained", history},
            {"valid", logs && current && bundle && history}};
    }
private:
    bool enabled_ = false, ready_ = true;
    std::filesystem::path root_;
    std::shared_ptr<livekit::diagnostic::DiagnosticPipeline> pipeline_;
    std::shared_ptr<livekit::telemetry::TelemetryHistoryStore> history_;
    std::unique_ptr<MeetingUI::DiagnosticQtBridge> qt_;
};
