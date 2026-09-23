#include <QtCore/QCoreApplication>
#include "src/core/meeting_coordinator.h"
#include "src/net/service_endpoint_policy.h"
#include "src/ui/meeting_log_console.h"
#include "src/telemetry/log_redaction.h"
#include "src/telemetry/process_resource_sampler.h"
#include "src/telemetry/stability_ledger.h"
#include "src/telemetry/telemetry_report.h"
#include "src/core/whiteboard/whiteboard_protocol.h"
#include "src/core/whiteboard/whiteboard_runtime.h"
#include "src/core/whiteboard/whiteboard_transport.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonDocument>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QThread>

#include <exception>
#include <future>
#include <limits>
#include <set>
#include <utility>

namespace OpenMeeting {

QVariantMap ProjectTelemetrySnapshot(
    const livekit::telemetry::Snapshot &snapshot) {
    QVariantMap result;
    result.insert(QStringLiteral("sessionGeneration"),
                  QVariant::fromValue<qulonglong>(snapshot.session_generation));
    result.insert(QStringLiteral("schemaVersion"),
                  livekit::telemetry::kTelemetryReportSchemaVersion);
    result.insert(QStringLiteral("definitionVersion"),
                  livekit::telemetry::kTelemetryDefinitionVersion);
    result.insert(QStringLiteral("revision"),
                  QVariant::fromValue<qulonglong>(snapshot.revision));
    result.insert(QStringLiteral("sessionComplete"), snapshot.session_complete);
    result.insert(QStringLiteral("availability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(snapshot.availability)));
    result.insert(QStringLiteral("reason"), QString::fromStdString(snapshot.reason));
    result.insert(QStringLiteral("sampleAgeMs"), snapshot.sample_age_ms);
    result.insert(QStringLiteral("coverage"), snapshot.coverage);
    result.insert(QStringLiteral("queueCapacity"),
                  QVariant::fromValue<qulonglong>(snapshot.queue_capacity));
    result.insert(QStringLiteral("queueDepth"),
                  QVariant::fromValue<qulonglong>(snapshot.queue_depth));
    result.insert(QStringLiteral("queueHighWater"),
                  QVariant::fromValue<qulonglong>(snapshot.queue_high_water));
    result.insert(QStringLiteral("capacityDrops"),
                  QVariant::fromValue<qulonglong>(snapshot.capacity_drops));
    result.insert(QStringLiteral("stoppedDrops"),
                  QVariant::fromValue<qulonglong>(snapshot.stopped_drops));
    result.insert(QStringLiteral("staleGenerationDrops"),
                  QVariant::fromValue<qulonglong>(snapshot.stale_generation_drops));
    result.insert(QStringLiteral("outOfOrderDrops"),
                  QVariant::fromValue<qulonglong>(snapshot.out_of_order_drops));
    result.insert(QStringLiteral("lateCallbacks"),
                  QVariant::fromValue<qulonglong>(snapshot.late_callbacks));
    result.insert(QStringLiteral("mappingFailures"),
                  QVariant::fromValue<qulonglong>(snapshot.mapping_failures));
    result.insert(QStringLiteral("counterResets"),
                  QVariant::fromValue<qulonglong>(snapshot.counter_resets));
    result.insert(QStringLiteral("validSamples"),
                  QVariant::fromValue<qulonglong>(snapshot.valid_samples));
    result.insert(QStringLiteral("unavailableSamples"),
                  QVariant::fromValue<qulonglong>(snapshot.unavailable_samples));
    result.insert(QStringLiteral("eventQueueLagAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(
            snapshot.event_queue_lag_availability)));
    result.insert(QStringLiteral("eventQueueLagReason"),
                  QString::fromStdString(snapshot.event_queue_lag_reason));
    result.insert(QStringLiteral("eventQueueLagSamples"),
                  QVariant::fromValue<qulonglong>(snapshot.event_queue_lag_samples));
    result.insert(QStringLiteral("lastEventQueueLagMs"),
                  snapshot.last_event_queue_lag_ms);
    result.insert(QStringLiteral("maximumEventQueueLagMs"),
                  snapshot.maximum_event_queue_lag_ms);
    result.insert(QStringLiteral("resourceAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(snapshot.resource_availability)));
    result.insert(QStringLiteral("resourceReason"),
                  QString::fromStdString(snapshot.resource_reason));
    result.insert(QStringLiteral("resourceSampleAgeMs"),
                  snapshot.resource_sample_age_ms);
    result.insert(QStringLiteral("resourceSamples"),
                  QVariant::fromValue<qulonglong>(snapshot.resource_samples));
    result.insert(QStringLiteral("resourceSampleFailures"),
                  QVariant::fromValue<qulonglong>(snapshot.resource_sample_failures));
    result.insert(QStringLiteral("lastResourceSampleUs"),
                  snapshot.last_resource_sample_us);
    result.insert(QStringLiteral("cpuAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(snapshot.cpu_availability)));
    result.insert(QStringLiteral("cpuReason"),
                  QString::fromStdString(snapshot.cpu_reason));
    result.insert(QStringLiteral("processCpuPercent"),
                  snapshot.process_cpu_percent);
    result.insert(QStringLiteral("logicalProcessorCount"),
                  snapshot.logical_processor_count);
    result.insert(QStringLiteral("memoryAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(snapshot.memory_availability)));
    result.insert(QStringLiteral("memoryReason"),
                  QString::fromStdString(snapshot.memory_reason));
    result.insert(QStringLiteral("workingSetBytes"),
                  QVariant::fromValue<qulonglong>(snapshot.working_set_bytes));
    result.insert(QStringLiteral("peakWorkingSetBytes"),
                  QVariant::fromValue<qulonglong>(snapshot.peak_working_set_bytes));
    result.insert(QStringLiteral("privateBytes"),
                  QVariant::fromValue<qulonglong>(snapshot.private_bytes));
    result.insert(QStringLiteral("threadCountAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(
            snapshot.thread_count_availability)));
    result.insert(QStringLiteral("threadCountReason"),
                  QString::fromStdString(snapshot.thread_count_reason));
    result.insert(QStringLiteral("processThreadCount"),
                  snapshot.process_thread_count);
    result.insert(QStringLiteral("threadCountSampleAgeMs"),
                  snapshot.thread_count_sample_age_ms);
    result.insert(QStringLiteral("handleCountAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(
            snapshot.handle_count_availability)));
    result.insert(QStringLiteral("handleCountReason"),
                  QString::fromStdString(snapshot.handle_count_reason));
    result.insert(QStringLiteral("processHandleCount"),
                  snapshot.process_handle_count);
    result.insert(QStringLiteral("gpuResourceAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(
            snapshot.gpu_resource_availability)));
    result.insert(QStringLiteral("gpuResourceReason"),
                  QString::fromStdString(snapshot.gpu_resource_reason));
    result.insert(QStringLiteral("resourceTrendAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(
            snapshot.resource_trend_availability)));
    result.insert(QStringLiteral("resourceTrendReason"),
                  QString::fromStdString(snapshot.resource_trend_reason));
    result.insert(QStringLiteral("resourceTrendSamples"),
                  QVariant::fromValue<qulonglong>(snapshot.resource_trend_samples));
    result.insert(QStringLiteral("resourceTrendSpanMs"),
                  snapshot.resource_trend_span_ms);
    result.insert(QStringLiteral("resourceTrendCoverage"),
                  snapshot.resource_trend_coverage);
    result.insert(QStringLiteral("minimumWorkingSetBytes"),
                  QVariant::fromValue<qulonglong>(snapshot.minimum_working_set_bytes));
    result.insert(QStringLiteral("maximumWorkingSetBytes"),
                  QVariant::fromValue<qulonglong>(snapshot.maximum_working_set_bytes));
    result.insert(QStringLiteral("minimumPrivateBytes"),
                  QVariant::fromValue<qulonglong>(snapshot.minimum_private_bytes));
    result.insert(QStringLiteral("maximumPrivateBytes"),
                  QVariant::fromValue<qulonglong>(snapshot.maximum_private_bytes));
    result.insert(QStringLiteral("minimumThreadCount"),
                  snapshot.minimum_thread_count);
    result.insert(QStringLiteral("maximumThreadCount"),
                  snapshot.maximum_thread_count);
    result.insert(QStringLiteral("minimumHandleCount"),
                  snapshot.minimum_handle_count);
    result.insert(QStringLiteral("maximumHandleCount"),
                  snapshot.maximum_handle_count);
    result.insert(QStringLiteral("privateBytesGrowthMibPerMinute"),
                  snapshot.private_bytes_growth_mib_per_minute);
    result.insert(QStringLiteral("threadGrowthPerHour"),
                  snapshot.thread_growth_per_hour);
    result.insert(QStringLiteral("handleGrowthPerHour"),
                  snapshot.handle_growth_per_hour);
    result.insert(QStringLiteral("resourceSessionDeltaAvailability"),
                  QString::fromLatin1(livekit::telemetry::AvailabilityName(
                      snapshot.resource_session_delta_availability)));
    result.insert(QStringLiteral("resourceSessionDeltaReason"),
                  QString::fromStdString(snapshot.resource_session_delta_reason));
    result.insert(QStringLiteral("workingSetDeltaBytes"),
                  snapshot.working_set_delta_bytes);
    result.insert(QStringLiteral("privateBytesDelta"),
                  snapshot.private_bytes_delta);
    result.insert(QStringLiteral("threadCountDelta"),
                  snapshot.thread_count_delta);
    result.insert(QStringLiteral("handleCountDelta"),
                  snapshot.handle_count_delta);
    result.insert(QStringLiteral("resourceFinalDeltaAvailability"),
                  QString::fromLatin1(livekit::telemetry::AvailabilityName(
                      snapshot.resource_final_delta_availability)));
    result.insert(QStringLiteral("resourceFinalDeltaReason"),
                  QString::fromStdString(snapshot.resource_final_delta_reason));
    result.insert(QStringLiteral("resourceReturnAvailability"),
                  QString::fromLatin1(livekit::telemetry::AvailabilityName(
                      snapshot.resource_return_availability)));
    result.insert(QStringLiteral("resourceReturnReason"),
                  QString::fromStdString(snapshot.resource_return_reason));
    result.insert(QStringLiteral("telemetryCostAvailability"),
                  QString::fromLatin1(livekit::telemetry::AvailabilityName(
                      snapshot.telemetry_cost_availability)));
    result.insert(QStringLiteral("telemetryCostReason"),
                  QString::fromStdString(snapshot.telemetry_cost_reason));
    result.insert(QStringLiteral("telemetrySnapshotPublications"),
                  QVariant::fromValue<qulonglong>(
                      snapshot.telemetry_snapshot_publications));
    result.insert(QStringLiteral("totalResourceSampleUs"),
                  snapshot.total_resource_sample_us);
    result.insert(QStringLiteral("maximumResourceSampleUs"),
                  snapshot.maximum_resource_sample_us);
    result.insert(QStringLiteral("averageResourceSampleUs"),
                  snapshot.average_resource_sample_us);
    result.insert(QStringLiteral("lastSnapshotBuildUs"),
                  snapshot.last_snapshot_build_us);
    result.insert(QStringLiteral("maximumSnapshotBuildUs"),
                  snapshot.maximum_snapshot_build_us);
    result.insert(QStringLiteral("totalSnapshotBuildUs"),
                  snapshot.total_snapshot_build_us);
    result.insert(QStringLiteral("lastSnapshotCallbackUs"),
                  snapshot.last_snapshot_callback_us);
    result.insert(QStringLiteral("maximumSnapshotCallbackUs"),
                  snapshot.maximum_snapshot_callback_us);
    result.insert(QStringLiteral("totalSnapshotCallbackUs"),
                  snapshot.total_snapshot_callback_us);
    result.insert(QStringLiteral("telemetryObservedCostRatio"),
                  snapshot.telemetry_observed_cost_ratio);
    result.insert(QStringLiteral("telemetryAbAvailability"),
                  QString::fromLatin1(livekit::telemetry::AvailabilityName(
                      snapshot.telemetry_ab_availability)));
    result.insert(QStringLiteral("telemetryAbReason"),
                  QString::fromStdString(snapshot.telemetry_ab_reason));
    if (const auto ledger = livekit::telemetry::InstalledStabilityLedger()) {
        const auto stability = ledger->Summary();
        result.insert(QStringLiteral("stabilityLedgerAvailability"),
                      QString::fromStdString(stability.ledger_availability));
        result.insert(QStringLiteral("stabilityLedgerReason"),
                      QString::fromStdString(stability.ledger_reason));
        result.insert(QStringLiteral("confirmedCrashAvailability"),
                      QString::fromStdString(
                          stability.confirmed_crash_availability));
        result.insert(QStringLiteral("confirmedCrashReason"),
                      QString::fromStdString(stability.confirmed_crash_reason));
        result.insert(QStringLiteral("unknownTerminationAvailability"),
                      QString::fromStdString(
                          stability.unknown_termination_availability));
        result.insert(QStringLiteral("unknownTerminationReason"),
                      QString::fromStdString(
                          stability.unknown_termination_reason));
        result.insert(QStringLiteral("processRunsStarted"),
                      QVariant::fromValue<qulonglong>(
                          stability.process_runs_started));
        result.insert(QStringLiteral("processRunsTerminal"),
                      QVariant::fromValue<qulonglong>(
                          stability.process_runs_terminal));
        result.insert(QStringLiteral("cleanProcessExits"),
                      QVariant::fromValue<qulonglong>(
                          stability.clean_process_exits));
        result.insert(QStringLiteral("unknownProcessTerminations"),
                      QVariant::fromValue<qulonglong>(
                          stability.unknown_process_terminations));
        result.insert(QStringLiteral("confirmedProcessCrashes"),
                      QVariant::fromValue<qulonglong>(
                          stability.confirmed_process_crashes));
        result.insert(QStringLiteral("sessionsStarted"),
                      QVariant::fromValue<qulonglong>(stability.sessions_started));
        result.insert(QStringLiteral("sessionsTerminal"),
                      QVariant::fromValue<qulonglong>(stability.sessions_terminal));
        result.insert(QStringLiteral("unknownSessionTerminations"),
                      QVariant::fromValue<qulonglong>(
                          stability.unknown_session_terminations));
        result.insert(QStringLiteral("stabilityCorruptInputs"),
                      QVariant::fromValue<qulonglong>(stability.corrupt_inputs));
        result.insert(QStringLiteral("stabilityWriteFailures"),
                      QVariant::fromValue<qulonglong>(stability.write_failures));
        result.insert(QStringLiteral("stabilityDuplicateTerminals"),
                      QVariant::fromValue<qulonglong>(
                          stability.duplicate_terminals));
        result.insert(QStringLiteral("unknownProcessTerminationRatio"),
                      stability.unknown_process_termination_ratio);
        result.insert(QStringLiteral("confirmedProcessCrashRatio"),
                      stability.confirmed_process_crash_ratio);
    } else {
        result.insert(QStringLiteral("stabilityLedgerAvailability"),
                      QStringLiteral("UNSUPPORTED"));
        result.insert(QStringLiteral("stabilityLedgerReason"),
                      QStringLiteral("process_ledger_not_installed"));
        result.insert(QStringLiteral("confirmedCrashAvailability"),
                      QStringLiteral("UNSUPPORTED"));
        result.insert(QStringLiteral("confirmedCrashReason"),
                      QStringLiteral("crash_evidence_provider_not_configured"));
        result.insert(QStringLiteral("unknownTerminationAvailability"),
                      QStringLiteral("UNKNOWN"));
        result.insert(QStringLiteral("unknownTerminationReason"),
                      QStringLiteral("process_ledger_not_installed"));
    }
    result.insert(QStringLiteral("strandLagAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(snapshot.strand_lag_availability)));
    result.insert(QStringLiteral("strandLagReason"),
                  QString::fromStdString(snapshot.strand_lag_reason));
    result.insert(QStringLiteral("strandLagSamples"),
                  QVariant::fromValue<qulonglong>(snapshot.strand_lag_samples));
    result.insert(QStringLiteral("lastStrandLagMs"), snapshot.last_strand_lag_ms);
    result.insert(QStringLiteral("maximumStrandLagMs"),
                  snapshot.maximum_strand_lag_ms);
    result.insert(QStringLiteral("uiLagAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(snapshot.ui_lag_availability)));
    result.insert(QStringLiteral("uiLagReason"),
                  QString::fromStdString(snapshot.ui_lag_reason));
    result.insert(QStringLiteral("uiLagSamples"),
                  QVariant::fromValue<qulonglong>(snapshot.ui_lag_samples));
    result.insert(QStringLiteral("lastUiLagMs"), snapshot.last_ui_lag_ms);
    result.insert(QStringLiteral("maximumUiLagMs"), snapshot.maximum_ui_lag_ms);
    result.insert(QStringLiteral("uiProbeTimeouts"),
                  QVariant::fromValue<qulonglong>(snapshot.ui_probe_timeouts));
    result.insert(QStringLiteral("uiProbeSkipped"),
                  QVariant::fromValue<qulonglong>(snapshot.ui_probe_skipped));
    result.insert(QStringLiteral("uiProbeLateCallbacks"),
                  QVariant::fromValue<qulonglong>(snapshot.ui_probe_late_callbacks));
    result.insert(QStringLiteral("uiProbeInFlight"), snapshot.ui_probe_in_flight);
    result.insert(QStringLiteral("statsInFlight"), snapshot.stats_in_flight);
    result.insert(QStringLiteral("actualPcCount"), snapshot.actual_pc_count);
    result.insert(QStringLiteral("successfulPcCount"), snapshot.successful_pc_count);
    result.insert(QStringLiteral("statsRequestsStarted"),
                  QVariant::fromValue<qulonglong>(snapshot.stats_requests_started));
    result.insert(QStringLiteral("statsRequestsCompleted"),
                  QVariant::fromValue<qulonglong>(snapshot.stats_requests_completed));
    result.insert(QStringLiteral("statsRequestTimeouts"),
                  QVariant::fromValue<qulonglong>(snapshot.stats_request_timeouts));
    result.insert(QStringLiteral("statsRequestRejections"),
                  QVariant::fromValue<qulonglong>(snapshot.stats_request_rejections));
    result.insert(QStringLiteral("statsRequestsSkipped"),
                  QVariant::fromValue<qulonglong>(snapshot.stats_requests_skipped));
    result.insert(QStringLiteral("lastStatsRequestMs"), snapshot.last_stats_request_ms);
    result.insert(QStringLiteral("operationsStarted"),
                  QVariant::fromValue<qulonglong>(snapshot.operations_started));
    result.insert(QStringLiteral("operationsTerminal"),
                  QVariant::fromValue<qulonglong>(snapshot.operations_terminal));
    result.insert(QStringLiteral("operationsInflight"),
                  QVariant::fromValue<qulonglong>(snapshot.operations_inflight));
    result.insert(QStringLiteral("operationsMissingStart"),
                  QVariant::fromValue<qulonglong>(snapshot.operations_missing_start));
    result.insert(QStringLiteral("operationsDuplicateTerminal"),
                  QVariant::fromValue<qulonglong>(snapshot.operations_duplicate_terminal));
    result.insert(QStringLiteral("operationsKindMismatch"),
                  QVariant::fromValue<qulonglong>(snapshot.operations_kind_mismatch));
    QVariantList operationSummaries;
    operationSummaries.reserve(static_cast<qsizetype>(snapshot.operation_summaries.size()));
    for (const auto &summary : snapshot.operation_summaries) {
        QVariantMap item;
        item.insert(QStringLiteral("kind"), QString::fromLatin1(
            livekit::telemetry::OperationKindName(summary.kind)));
        item.insert(QStringLiteral("started"),
                    QVariant::fromValue<qulonglong>(summary.started));
        item.insert(QStringLiteral("terminal"),
                    QVariant::fromValue<qulonglong>(summary.terminal));
        item.insert(QStringLiteral("success"),
                    QVariant::fromValue<qulonglong>(summary.success));
        item.insert(QStringLiteral("degradedSuccess"),
                    QVariant::fromValue<qulonglong>(summary.degraded_success));
        item.insert(QStringLiteral("failure"),
                    QVariant::fromValue<qulonglong>(summary.failure));
        item.insert(QStringLiteral("timeout"),
                    QVariant::fromValue<qulonglong>(summary.timeout));
        item.insert(QStringLiteral("cancelled"),
                    QVariant::fromValue<qulonglong>(summary.cancelled));
        item.insert(QStringLiteral("inflight"),
                    QVariant::fromValue<qulonglong>(summary.inflight));
        item.insert(QStringLiteral("lastDurationMs"), summary.last_duration_ms);
        operationSummaries.push_back(std::move(item));
    }
    result.insert(QStringLiteral("operationSummaries"), operationSummaries);
    result.insert(QStringLiteral("firstVideoAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(
            snapshot.remote_video_first_frame_availability)));
    result.insert(QStringLiteral("firstVideoReason"),
                  QString::fromStdString(snapshot.remote_video_first_frame_reason));
    result.insert(QStringLiteral("firstVideoMeasurementPoint"),
                  QString::fromStdString(
                      snapshot.remote_video_first_frame_measurement_point));
    result.insert(QStringLiteral("remoteVideoBindings"),
                  QVariant::fromValue<qulonglong>(snapshot.remote_video_bindings));
    result.insert(QStringLiteral("remoteVideoFirstFrames"),
                  QVariant::fromValue<qulonglong>(snapshot.remote_video_first_frames));
    result.insert(QStringLiteral("staleBindingFrameDrops"),
                  QVariant::fromValue<qulonglong>(snapshot.stale_binding_frame_drops));
    result.insert(QStringLiteral("roomConnectToFirstDecodedMs"),
                  snapshot.room_connect_to_first_decoded_ms);
    result.insert(QStringLiteral("lastConnectToFirstDecodedMs"),
                  snapshot.last_connect_to_first_decoded_ms);
    result.insert(QStringLiteral("lastSubscribeToFirstDecodedMs"),
                  snapshot.last_subscribe_to_first_decoded_ms);
    result.insert(QStringLiteral("lastDecodedWidth"), snapshot.last_decoded_width);
    result.insert(QStringLiteral("lastDecodedHeight"), snapshot.last_decoded_height);
    result.insert(QStringLiteral("nativeVideoFreezeAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(
            snapshot.native_video_freeze_availability)));
    result.insert(QStringLiteral("nativeVideoFreezeReason"),
                  QString::fromStdString(snapshot.native_video_freeze_reason));
    result.insert(QStringLiteral("nativeVideoFreezeMeasurementPoint"),
                  QString::fromStdString(
                      snapshot.native_video_freeze_measurement_point));
    result.insert(QStringLiteral("nativeVideoStreams"),
                  QVariant::fromValue<qulonglong>(snapshot.native_video_streams));
    result.insert(QStringLiteral("nativeVideoFreezeCount"),
                  QVariant::fromValue<qulonglong>(snapshot.native_video_freeze_count));
    result.insert(QStringLiteral("nativeVideoFreezeDurationMs"),
                  snapshot.native_video_freeze_duration_ms);
    result.insert(QStringLiteral("reconnectVideoAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(
            snapshot.reconnect_video_availability)));
    result.insert(QStringLiteral("reconnectVideoReason"),
                  QString::fromStdString(snapshot.reconnect_video_reason));
    result.insert(QStringLiteral("reconnectVideoMeasurementPoint"),
                  QString::fromStdString(
                      snapshot.reconnect_video_measurement_point));
    result.insert(QStringLiteral("reconnectVideoExpected"),
                  QVariant::fromValue<qulonglong>(snapshot.reconnect_video_expected));
    result.insert(QStringLiteral("reconnectVideoRecovered"),
                  QVariant::fromValue<qulonglong>(snapshot.reconnect_video_recovered));
    result.insert(QStringLiteral("reconnectExpectationChanges"),
                  QVariant::fromValue<qulonglong>(snapshot.reconnect_expectation_changes));
    result.insert(QStringLiteral("reconnectMediaTimeouts"),
                  QVariant::fromValue<qulonglong>(snapshot.reconnect_media_timeouts));
    result.insert(QStringLiteral("lastReconnectSignalingMs"),
                  snapshot.last_reconnect_signaling_ms);
    result.insert(QStringLiteral("lastReconnectFirstVideoMs"),
                  snapshot.last_reconnect_first_video_ms);
    result.insert(QStringLiteral("lastReconnectStableVideoMs"),
                  snapshot.last_reconnect_stable_video_ms);
    result.insert(QStringLiteral("lastReconnectMediaInterruptionMs"),
                  snapshot.last_reconnect_media_interruption_ms);
    result.insert(QStringLiteral("firstAudioAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(
            snapshot.remote_audio_first_frame_availability)));
    result.insert(QStringLiteral("firstAudioReason"),
                  QString::fromStdString(snapshot.remote_audio_first_frame_reason));
    result.insert(QStringLiteral("firstAudioMeasurementPoint"),
                  QString::fromStdString(
                      snapshot.remote_audio_first_frame_measurement_point));
    result.insert(QStringLiteral("remoteAudioBindings"),
                  QVariant::fromValue<qulonglong>(snapshot.remote_audio_bindings));
    result.insert(QStringLiteral("remoteAudioFirstFrames"),
                  QVariant::fromValue<qulonglong>(snapshot.remote_audio_first_frames));
    result.insert(QStringLiteral("staleAudioBindingDrops"),
                  QVariant::fromValue<qulonglong>(snapshot.stale_audio_binding_drops));
    result.insert(QStringLiteral("lastSubscribeToFirstPcmMs"),
                  snapshot.last_subscribe_to_first_pcm_ms);
    result.insert(QStringLiteral("lastAudioSampleRate"), snapshot.last_audio_sample_rate);
    result.insert(QStringLiteral("lastAudioChannels"), snapshot.last_audio_channels);
    result.insert(QStringLiteral("audioQualityAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(snapshot.audio_quality_availability)));
    result.insert(QStringLiteral("audioQualityReason"),
                  QString::fromStdString(snapshot.audio_quality_reason));
    result.insert(QStringLiteral("audioQualityMeasurementPoint"),
                  QString::fromStdString(snapshot.audio_quality_measurement_point));
    result.insert(QStringLiteral("audioConcealmentAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(
            snapshot.audio_concealment_availability)));
    result.insert(QStringLiteral("audioConcealmentReason"),
                  QString::fromStdString(snapshot.audio_concealment_reason));
    result.insert(QStringLiteral("audioJitterBufferAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(
            snapshot.audio_jitter_buffer_availability)));
    result.insert(QStringLiteral("audioJitterBufferReason"),
                  QString::fromStdString(snapshot.audio_jitter_buffer_reason));
    result.insert(QStringLiteral("audioTimeStretchAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(
            snapshot.audio_time_stretch_availability)));
    result.insert(QStringLiteral("audioTimeStretchReason"),
                  QString::fromStdString(snapshot.audio_time_stretch_reason));
    result.insert(QStringLiteral("audioStreams"),
                  QVariant::fromValue<qulonglong>(snapshot.audio_streams));
    result.insert(QStringLiteral("audioWindowSamples"),
                  QVariant::fromValue<qulonglong>(snapshot.audio_window_samples));
    result.insert(QStringLiteral("audioWindowConcealedSamples"),
                  QVariant::fromValue<qulonglong>(snapshot.audio_window_concealed_samples));
    result.insert(QStringLiteral("audioWindowSilentConcealedSamples"),
                  QVariant::fromValue<qulonglong>(
                      snapshot.audio_window_silent_concealed_samples));
    result.insert(QStringLiteral("audioWindowConcealmentEvents"),
                  QVariant::fromValue<qulonglong>(
                      snapshot.audio_window_concealment_events));
    result.insert(QStringLiteral("audioConcealedRatio"), snapshot.audio_concealed_ratio);
    result.insert(QStringLiteral("audioNonSilentConcealedRatio"),
                  snapshot.audio_non_silent_concealed_ratio);
    result.insert(QStringLiteral("audioJitterBufferDelayMs"),
                  snapshot.audio_jitter_buffer_delay_ms);
    result.insert(QStringLiteral("audioJitterBufferTargetDelayMs"),
                  snapshot.audio_jitter_buffer_target_delay_ms);
    result.insert(QStringLiteral("audioJitterBufferMinimumDelayMs"),
                  snapshot.audio_jitter_buffer_minimum_delay_ms);
    result.insert(QStringLiteral("audioInsertedSamples"),
                  QVariant::fromValue<qulonglong>(snapshot.audio_inserted_samples));
    result.insert(QStringLiteral("audioRemovedSamples"),
                  QVariant::fromValue<qulonglong>(snapshot.audio_removed_samples));
    result.insert(QStringLiteral("audioInsertedRatio"), snapshot.audio_inserted_ratio);
    result.insert(QStringLiteral("audioRemovedRatio"), snapshot.audio_removed_ratio);
    result.insert(QStringLiteral("reconnectAudioAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(snapshot.reconnect_audio_availability)));
    result.insert(QStringLiteral("reconnectAudioReason"),
                  QString::fromStdString(snapshot.reconnect_audio_reason));
    result.insert(QStringLiteral("reconnectAudioExpected"),
                  QVariant::fromValue<qulonglong>(snapshot.reconnect_audio_expected));
    result.insert(QStringLiteral("reconnectAudioRecovered"),
                  QVariant::fromValue<qulonglong>(snapshot.reconnect_audio_recovered));
    result.insert(QStringLiteral("lastReconnectFirstAudioMs"),
                  snapshot.last_reconnect_first_audio_ms);
    result.insert(QStringLiteral("lastReconnectStableAudioMs"),
                  snapshot.last_reconnect_stable_audio_ms);
    result.insert(QStringLiteral("lastReconnectAudioInterruptionMs"),
                  snapshot.last_reconnect_audio_interruption_ms);
    result.insert(QStringLiteral("renderFirstFrameAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(snapshot.render_first_frame_availability)));
    result.insert(QStringLiteral("renderFirstFrameReason"),
                  QString::fromStdString(snapshot.render_first_frame_reason));
    result.insert(QStringLiteral("renderMeasurementPoint"),
                  QString::fromStdString(snapshot.render_first_frame_measurement_point));
    result.insert(QStringLiteral("renderBindings"),
                  QVariant::fromValue<qulonglong>(snapshot.render_bindings));
    result.insert(QStringLiteral("uniqueRenderSubmits"),
                  QVariant::fromValue<qulonglong>(snapshot.unique_render_submits));
    result.insert(QStringLiteral("staleRenderBindingDrops"),
                  QVariant::fromValue<qulonglong>(snapshot.stale_render_binding_drops));
    result.insert(QStringLiteral("lastDecodeToRenderMs"), snapshot.last_decode_to_render_ms);
    result.insert(QStringLiteral("lastSubscribeToFirstRenderMs"),
                  snapshot.last_subscribe_to_first_render_ms);
    result.insert(QStringLiteral("lastConnectToFirstRenderMs"),
                  snapshot.last_connect_to_first_render_ms);
    result.insert(QStringLiteral("renderAverageIntervalMs"),
                  snapshot.render_average_interval_ms);
    result.insert(QStringLiteral("renderMaximumIntervalMs"),
                  snapshot.render_maximum_interval_ms);
    result.insert(QStringLiteral("renderStallAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(snapshot.render_stall_availability)));
    result.insert(QStringLiteral("renderStallReason"),
                  QString::fromStdString(snapshot.render_stall_reason));
    result.insert(QStringLiteral("renderStallAlgorithm"),
                  QString::fromStdString(snapshot.render_stall_algorithm));
    result.insert(QStringLiteral("renderStallCount"),
                  QVariant::fromValue<qulonglong>(snapshot.render_stall_count));
    result.insert(QStringLiteral("renderStallDurationMs"),
                  snapshot.render_stall_duration_ms);
    result.insert(QStringLiteral("renderLongestStallMs"),
                  snapshot.render_longest_stall_ms);
    result.insert(QStringLiteral("renderExpectedDurationMs"),
                  snapshot.render_expected_duration_ms);
    result.insert(QStringLiteral("renderStallRatio"), snapshot.render_stall_ratio);
    result.insert(QStringLiteral("renderStallActive"), snapshot.render_stall_active);
    result.insert(QStringLiteral("reconnectRenderAvailability"), QString::fromLatin1(
        livekit::telemetry::AvailabilityName(snapshot.reconnect_render_availability)));
    result.insert(QStringLiteral("reconnectRenderReason"),
                  QString::fromStdString(snapshot.reconnect_render_reason));
    result.insert(QStringLiteral("reconnectRenderExpected"),
                  QVariant::fromValue<qulonglong>(snapshot.reconnect_render_expected));
    result.insert(QStringLiteral("reconnectRenderRecovered"),
                  QVariant::fromValue<qulonglong>(snapshot.reconnect_render_recovered));
    result.insert(QStringLiteral("lastReconnectFirstRenderMs"),
                  snapshot.last_reconnect_first_render_ms);
    result.insert(QStringLiteral("lastReconnectStableRenderMs"),
                  snapshot.last_reconnect_stable_render_ms);
    result.insert(QStringLiteral("lastReconnectRenderInterruptionMs"),
                  snapshot.last_reconnect_render_interruption_ms);
    if (const auto store = livekit::telemetry::InstalledTelemetryHistoryStore()) {
        const auto status = store->Status();
        result.insert(QStringLiteral("telemetryStorageAvailability"),
                      QString::fromLatin1(livekit::telemetry::AvailabilityName(
                          status->availability)));
        result.insert(QStringLiteral("telemetryStorageReason"),
                      QString::fromStdString(status->reason));
        result.insert(QStringLiteral("telemetryHistoryEnabled"),
                      status->history_enabled);
        result.insert(QStringLiteral("telemetryHistoryRecords"),
                      QVariant::fromValue<qulonglong>(status->reports.size()));
        result.insert(QStringLiteral("telemetryHistoryMemoryBuckets"),
                      QVariant::fromValue<qulonglong>(status->memory_records));
        result.insert(QStringLiteral("telemetryHistoryQueueDrops"),
                      QVariant::fromValue<qulonglong>(status->queue_drops));
        result.insert(QStringLiteral("telemetryHistoryWriteFailures"),
                      QVariant::fromValue<qulonglong>(status->write_failures));
        result.insert(QStringLiteral("telemetryHistoryCorruptReports"),
                      QVariant::fromValue<qulonglong>(status->corrupt_reports));
    } else {
        result.insert(QStringLiteral("telemetryStorageAvailability"),
                      QStringLiteral("UNSUPPORTED"));
        result.insert(QStringLiteral("telemetryStorageReason"),
                      QStringLiteral("history_store_not_installed"));
        result.insert(QStringLiteral("telemetryHistoryEnabled"), false);
    }
    return result;
}

// 从 LiveKit Participant 中提取真实用户昵称 (优先解析 OpenMeeting metadata 中的 JSON userInfo.nickname)
static QString ResolveParticipantNickname(const livekit::ParticipantStateSnapshot &state) {
    QString identity = QString::fromStdString(state.identity).trimmed();
    QString metaStr = QString::fromStdString(state.metadata).trimmed();
    if (!metaStr.isEmpty()) {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(metaStr.toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            QJsonObject obj = doc.object();
            // 1. userInfo.nickname (OpenMeeting 核心标准)
            if (obj.contains("userInfo") && obj["userInfo"].isObject()) {
                QJsonObject userObj = obj["userInfo"].toObject();
                QString nick = userObj["nickname"].toString().trimmed();
                if (!nick.isEmpty()) return nick;
                QString uName = userObj["name"].toString().trimmed();
                if (!uName.isEmpty() && uName != "participant-name") return uName;
            }
            // 2. 根级 nickname
            QString rootNick = obj["nickname"].toString().trimmed();
            if (!rootNick.isEmpty()) return rootNick;
            // 3. 根级 name (排除 participant-name)
            QString rootName = obj["name"].toString().trimmed();
            if (!rootName.isEmpty() && rootName != "participant-name") return rootName;
        }
    }
    // 4. Participant 原生 name (排除 participant-name)
    QString rawName = QString::fromStdString(state.name).trimmed();
    if (!rawName.isEmpty() && rawName != "participant-name") {
        return rawName;
    }
    // 5. 回退 identity (纯数字 ID)
    return identity;
}

static QString ResolveParticipantNickname(const std::shared_ptr<livekit::Participant> &p) {
    return p ? ResolveParticipantNickname(p->SnapshotState()) : QString();
}

static MeetingRoomInfo ToMeetingRoomInfo(const livekit::RoomInfo &info) {
    MeetingRoomInfo result;
    result.sid = QString::fromStdString(info.sid);
    result.name = QString::fromStdString(info.name);
    result.metadata = QString::fromStdString(info.metadata);
    result.emptyTimeout = info.empty_timeout;
    result.departureTimeout = info.departure_timeout;
    result.maxParticipants = info.max_participants;
    result.creationTimeMs = info.creation_time_ms;
    result.numParticipants = info.num_participants;
    result.numPublishers = info.num_publishers;
    result.activeRecording = info.active_recording;
    return result;
}

enum class OpenMeetingMetadataState {
    Absent,
    Valid,
    Invalid,
};

struct ParsedOpenMeetingMetadata {
    OpenMeetingMetadataState state = OpenMeetingMetadataState::Absent;
    MeetingDetail detail;
};

static bool ReadOptionalString(
        const QJsonObject &object, const char *key, QString *value) {
    if (!object.contains(key)) return true;
    const auto candidate = object.value(key);
    if (!candidate.isString()) return false;
    *value = candidate.toString();
    return true;
}

static bool ReadOptionalInt64(
        const QJsonObject &object, const char *key, int64_t *value) {
    if (!object.contains(key)) return true;
    const auto candidate = object.value(key);
    if (candidate.isDouble()) {
        *value = candidate.toVariant().toLongLong();
        return true;
    }
    if (candidate.isString()) {
        bool ok = false;
        const auto parsed = candidate.toString().toLongLong(&ok);
        if (ok) {
            *value = parsed;
            return true;
        }
    }
    return false;
}

static bool ReadOptionalBool(
        const QJsonObject &object, const char *key, bool *value) {
    if (!object.contains(key)) return true;
    const auto candidate = object.value(key);
    if (!candidate.isBool()) return false;
    *value = candidate.toBool();
    return true;
}

static bool ReadOptionalObject(
        const QJsonObject &object, const char *key, QJsonObject *value) {
    if (!object.contains(key)) return true;
    const auto candidate = object.value(key);
    if (!candidate.isObject()) return false;
    *value = candidate.toObject();
    return true;
}

static ParsedOpenMeetingMetadata ParseOpenMeetingMetadata(const std::string &metadata) {
    ParsedOpenMeetingMetadata result;
    if (metadata.empty()) return result;

    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray::fromRawData(metadata.data(), static_cast<int>(metadata.size())), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return result;

    const auto root = document.object();
    if (!root.contains(QStringLiteral("detail"))) return result;
    result.state = OpenMeetingMetadataState::Invalid;
    if (!root.value(QStringLiteral("detail")).isObject()) return result;

    const auto detailObject = root.value(QStringLiteral("detail")).toObject();
    QJsonObject info;
    QJsonObject setting;
    QJsonObject systemGenerated;
    QJsonObject creatorDefined;
    if (!ReadOptionalObject(detailObject, "info", &info) ||
        !ReadOptionalObject(detailObject, "setting", &setting) ||
        !ReadOptionalObject(info, "systemGenerated", &systemGenerated) ||
        !ReadOptionalObject(info, "creatorDefinedMeeting", &creatorDefined)) {
        return result;
    }

    auto parsed = MeetingDetail{};
    parsed.canJoinEarly = false;
    int64_t scheduledTime = 0;
    int64_t duration = 0;
    if (!ReadOptionalString(systemGenerated, "meetingID", &parsed.meetingId) ||
        !ReadOptionalString(systemGenerated, "creatorUserID", &parsed.creatorUserId) ||
        !ReadOptionalInt64(systemGenerated, "startTime", &parsed.startTime) ||
        !ReadOptionalString(creatorDefined, "title", &parsed.meetingName) ||
        !ReadOptionalString(creatorDefined, "hostUserID", &parsed.hostUserId) ||
        !ReadOptionalInt64(creatorDefined, "scheduledTime", &scheduledTime) ||
        !ReadOptionalInt64(creatorDefined, "meetingDuration", &duration)) {
        return result;
    }

    // Older clients and fixtures used a flat detail.info shape. Preserve that
    // input while preferring the server's nested protobuf JSON structure.
    QString legacyString;
    int64_t legacyTime = 0;
    if (parsed.meetingId.isEmpty()) {
        legacyString.clear();
        if (!ReadOptionalString(info, "meetingID", &legacyString)) return result;
        parsed.meetingId = legacyString;
    }
    if (parsed.meetingName.isEmpty()) {
        legacyString.clear();
        if (!ReadOptionalString(info, "meetingName", &legacyString)) return result;
        parsed.meetingName = legacyString;
    }
    if (parsed.creatorUserId.isEmpty()) {
        legacyString.clear();
        if (!ReadOptionalString(info, "creatorUserID", &legacyString)) return result;
        parsed.creatorUserId = legacyString;
    }
    if (parsed.hostUserId.isEmpty()) {
        legacyString.clear();
        if (!ReadOptionalString(info, "hostUserID", &legacyString)) return result;
        parsed.hostUserId = legacyString;
    }
    if (parsed.startTime == 0) {
        if (!ReadOptionalInt64(info, "startTime", &legacyTime)) return result;
        parsed.startTime = legacyTime != 0 ? legacyTime : scheduledTime;
    }
    if (!ReadOptionalInt64(info, "endTime", &parsed.endTime)) return result;
    if (parsed.endTime == 0 && parsed.startTime > 0 && duration > 0 &&
        duration <= std::numeric_limits<int64_t>::max() - parsed.startTime) {
        parsed.endTime = parsed.startTime + duration;
    }
    if (parsed.hostUserId.isEmpty()) parsed.hostUserId = parsed.creatorUserId;

    if (!ReadOptionalBool(setting, "disableMicrophoneOnJoin", &parsed.disableMicrophoneOnJoin) ||
        !ReadOptionalBool(setting, "disableCameraOnJoin", &parsed.disableCameraOnJoin) ||
        !ReadOptionalBool(setting, "lockMeeting", &parsed.lockMeeting) ||
        !ReadOptionalBool(setting, "canParticipantJoinMeetingEarly", &parsed.canJoinEarly)) {
        return result;
    }

    result.state = OpenMeetingMetadataState::Valid;
    result.detail = std::move(parsed);
    return result;
}

InitialMediaState MeetingCoordinator::resolveInitialMediaState(
        const std::string &metadata,
        const MediaPreferences &preferences) {
    InitialMediaState result;
    result.microphoneEnabled = preferences.enableMicrophone;
    result.videoEnabled = preferences.enableVideo;

    const auto parsed = ParseOpenMeetingMetadata(metadata);
    result.hasOpenMeetingDetail = parsed.state != OpenMeetingMetadataState::Absent;
    result.metadataValid = parsed.state != OpenMeetingMetadataState::Invalid;
    if (parsed.state == OpenMeetingMetadataState::Invalid) {
        result.microphoneEnabled = false;
        result.videoEnabled = false;
        return result;
    }
    if (parsed.state == OpenMeetingMetadataState::Valid) {
        result.detail = parsed.detail;
        result.microphoneEnabled = preferences.enableMicrophone &&
            !parsed.detail.disableMicrophoneOnJoin;
        result.videoEnabled = preferences.enableVideo &&
            !parsed.detail.disableCameraOnJoin;
    }
    return result;
}

static void CopyParticipantState(const livekit::ParticipantStateSnapshot &state,
                                 ParticipantInfo *info) {
    if (!info) return;
    info->connectionQuality = state.connection_quality;
    info->connectionQualityScore = state.connection_quality_score;
    info->permissions = state.permission;

    bool audio_paused = false;
    bool video_paused = false;
    for (const auto &publication : state.publications) {
        if (!publication.track ||
            publication.stream_state != livekit::TrackPublication::StreamState::Paused) {
            continue;
        }
        if (publication.kind == livekit::TrackKind::Audio) {
            audio_paused = true;
        } else if (publication.kind == livekit::TrackKind::Video) {
            video_paused = true;
        }
    }
    info->isAudioStreamPaused = audio_paused;
    info->isVideoStreamPaused = video_paused;
}

static void CopyParticipantState(const std::shared_ptr<livekit::Participant> &participant,
                                 ParticipantInfo *info) {
    if (participant) CopyParticipantState(participant->SnapshotState(), info);
}

static QString ParticipantKeyToken(const livekit::ParticipantKey &key) {
    return QString::number(key.native_room_generation) + QLatin1Char(':') +
        QString::number(key.incarnation) + QLatin1Char(':') +
        QString::number(static_cast<qulonglong>(key.sid.size())) + QLatin1Char(':') +
        QString::fromStdString(key.sid) + QLatin1Char(':') +
        QString::fromStdString(key.identity);
}

// ----------------------------------------------------
// CoordinatorRoomListener: LiveKit 事件监听与桥接
// ----------------------------------------------------
class MeetingCoordinator::CoordinatorRoomListener : public livekit::RoomListener {
public:
    CoordinatorRoomListener(MeetingCoordinator *c,
                            const std::shared_ptr<MeetingSessionRuntime> &session)
        : _coordinator(c),
          _session(session),
          _generation(session ? session->generation() : 0),
          _authGeneration(c->_sessionManager.authGeneration()),
          _localUserId(session ? session->localUserId() : QString()) {}

    bool ConsumesParticipantEvents() const override { return true; }

    void OnParticipantEvent(const livekit::ParticipantEvent &event) override {
        auto *coordinator = _coordinator;
        if (!coordinator || _generation == 0) return;
        if (event.kind == livekit::ParticipantEventKind::DataReceived &&
            livekit::whiteboard::isWhiteboardTopic(event.topic)) {
            auto session = _session.lock();
            if (!session || event.sender.origin == livekit::SenderOrigin::Server ||
                !livekit::IsParticipantTicketActive(event.sender.ticket, event.sender.key)) return;
            coordinator->enqueueWhiteboardData(session, event.data, event.topic, event.sender);
            return;
        }
        if (event.kind == livekit::ParticipantEventKind::Upsert ||
            event.kind == livekit::ParticipantEventKind::Departure) {
            if (auto session = _session.lock()) {
                const auto key = event.participant.key;
                const auto ticket = event.participant.ticket;
                const bool departure = event.kind == livekit::ParticipantEventKind::Departure;
                asio::post(session->strand(), [session, key, ticket, departure] {
                    const std::string identity = key.identity.empty() ? key.sid : key.identity;
                    auto &peers = session->whiteboardPeersOnStrand();
                    auto &departures = session->whiteboardDeparturesOnStrand();
                    if (departure) {
                        const auto current = peers.find(identity);
                        if (current == peers.end() || current->second == key) peers.erase(identity);
                        departures[identity] = key;
                        if (departures.size() > 512) departures.erase(departures.begin());
                    } else if (livekit::IsParticipantTicketActive(ticket, key)) {
                        peers[identity] = key;
                        departures.erase(identity);
                    } else return;
                    auto runtime = session->whiteboardOnStrand();
                    if (!runtime) return;
                    livekit::whiteboard::PeerInstance peer{
                        identity, key.native_room_generation, key.incarnation};
                    if (departure) runtime->peerLeft(peer);
                    else runtime->observePeer(peer);
                });
            }
        }
        const auto generation = _generation;
        const auto authGeneration = _authGeneration;
        const auto localUserId = _localUserId;
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, authGeneration, localUserId, event]() {
            if (coordinator->applyAccountNotificationOnUiThread(generation, authGeneration, localUserId, event)) return;
            coordinator->applyParticipantEventOnUiThread(generation, event);
        }, Qt::QueuedConnection);
    }

    void OnConnected() override {
        auto *coordinator = _coordinator;
        if (!coordinator || _generation == 0) return;
        const auto generation = _generation;
        QMetaObject::invokeMethod(coordinator, [coordinator, generation]() {
            QPointer<MeetingCoordinator> owner(coordinator);
            if (!owner->isCurrentSessionGenerationOnUiThread(generation)) return;
            const auto nativeGeneration = owner->_nativeRoomGeneration;

            // ConnectAsync only establishes the Room transport.  The startup
            // transaction publishes required local tracks before it reports a
            // successful meeting join.
            // 读取 Room 的统一原生快照。JoinResponse 与后续 RoomUpdate
            // 都已经提交到这里，避免 UI 从过期 protobuf 副本读取状态。
            if (owner->_room) {
                const auto native_room_info = owner->_room->room_info();
                const auto info = ToMeetingRoomInfo(native_room_info);
                owner->_roomInfo = info;
                emit owner->roomInfoUpdated(info);
                if (!owner || !owner->isCurrentSessionGenerationOnUiThread(generation) ||
                    owner->_nativeRoomGeneration != nativeGeneration) return;
                owner->parseRoomMetadata(native_room_info.metadata);

            }
        }, Qt::QueuedConnection);
    }

    void OnDisconnected(livekit::RoomDisconnectReason reason,
                        const std::string &detail) override {
        if (auto session = _session.lock()) {
            asio::post(session->strand(), [session] {
                if (auto share = session->screenShareOnStrand()) share->Shutdown();
                if (auto board = session->whiteboardOnStrand()) board->setTransportReady(false, 0);
            });
        }
        auto *coordinator = _coordinator;
        if (!coordinator || _generation == 0) return;
        const auto generation = _generation;
        const QString qDetail = detail.empty()
            ? QCoreApplication::translate("MeetingUI", "No additional disconnect details")
            : QString::fromStdString(livekit::secure_log::OpaqueSummary("room_disconnect"));
        // 不捕获 listener 自身：DuplicateIdentity 处理会释放 _roomListener，
        // 捕获 this 会在回调执行期间留下悬垂指针风险。
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, reason, qDetail]() {
            QPointer<MeetingCoordinator> owner(coordinator);
            if (!owner->isCurrentSessionGenerationOnUiThread(generation)) return;
            const auto nativeGeneration = owner->_nativeRoomGeneration;
            const auto current = [&] {
                return owner && owner->isCurrentSessionGenerationOnUiThread(generation) &&
                    owner->_nativeRoomGeneration == nativeGeneration;
            };
            MeetingUI::LogToConsole(
                MeetingUI::LogCategory::Connection,
                "DISCONNECTED",
                QCoreApplication::translate("MeetingUI", "Disconnected from the LiveKit room: reason=%1, detail=%2")
                    .arg(QString::fromLatin1(livekit::ToString(reason)), qDetail));
            if (!current()) return;
            if (reason == livekit::RoomDisconnectReason::DuplicateIdentity) {
                owner->handleDuplicateIdentityKickOff(QString());
                return;
            }
            if (owner->_state != MeetingState::Leaving && owner->_state != MeetingState::Idle) {
                owner->_screenShareSnapshot = {};
                emit owner->screenShareChanged({});
                if (!current()) return;
                owner->setState(MeetingState::Idle, qDetail);
                if (!current() || owner->_state != MeetingState::Idle) return;
                emit owner->meetingLeft();
            }
        }, Qt::QueuedConnection);
    }

    void OnReconnecting() override {
        if (auto session = _session.lock()) {
            asio::post(session->strand(), [session] {
                if (auto share = session->screenShareOnStrand()) share->SetTransportReady(false);
                if (auto board = session->whiteboardOnStrand()) board->setTransportReady(false, 0);
            });
        }
        auto *coordinator = _coordinator;
        if (!coordinator || _generation == 0) return;
        const auto generation = _generation;
        QMetaObject::invokeMethod(coordinator, [coordinator, generation]() {
            QPointer<MeetingCoordinator> owner(coordinator);
            if (!owner->isCurrentSessionGenerationOnUiThread(generation) ||
                owner->_state == MeetingState::Leaving ||
                owner->_state == MeetingState::Idle) {
                return;
            }
            const auto nativeGeneration = owner->_nativeRoomGeneration;
            MeetingUI::LogToConsole(MeetingUI::LogCategory::Connection, "RECONNECTING",
                                    QCoreApplication::translate("MeetingUI", "Network interrupted. Restoring the audio/video connection."));
            if (!owner || !owner->isCurrentSessionGenerationOnUiThread(generation) ||
                owner->_nativeRoomGeneration != nativeGeneration ||
                owner->_state == MeetingState::Leaving || owner->_state == MeetingState::Idle) return;
            owner->_startupReconnectPending = true;
            owner->setState(MeetingState::Reconnecting,
                                  QCoreApplication::translate("MeetingUI", "Network interrupted. Reconnecting..."));
        }, Qt::QueuedConnection);
    }

    void OnReconnected() override {
        if (auto session = _session.lock()) {
            asio::post(session->strand(), [session] {
                if (auto share = session->screenShareOnStrand()) share->SetTransportReady(true);
                if (auto board = session->whiteboardOnStrand()) {
                    board->setTransportReady(true, static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch()));
                }
            });
        }
        auto *coordinator = _coordinator;
        if (!coordinator || _generation == 0) return;
        const auto generation = _generation;
        QMetaObject::invokeMethod(coordinator, [coordinator, generation]() {
            QPointer<MeetingCoordinator> owner(coordinator);
            if (!owner->isCurrentSessionGenerationOnUiThread(generation) ||
                owner->_state == MeetingState::Leaving ||
                owner->_state == MeetingState::Idle) {
                return;
            }
            const auto nativeGeneration = owner->_nativeRoomGeneration;
            owner->_startupReconnectPending = false;
            if (!owner->tryCommitOperationalStateOnUiThread(
                    generation, QCoreApplication::translate("MeetingUI", "Audio/video connection restored"))) {
                return;
            }
            if (!owner->_startupCommitted) {
                MeetingUI::LogToConsole(
                    MeetingUI::LogCategory::Connection,
                    "RECONNECTED_DURING_STARTUP",
                    QCoreApplication::translate("MeetingUI", "Connection restored. Waiting for local media startup to complete."));
                return;
            }
            if (!owner || !owner->isCurrentSessionGenerationOnUiThread(generation) ||
                owner->_nativeRoomGeneration != nativeGeneration ||
                owner->_state != MeetingState::InMeeting) return;
            MeetingUI::LogToConsole(MeetingUI::LogCategory::Connection, "RECONNECTED",
                                    QCoreApplication::translate("MeetingUI", "Audio/video connection restored. Waiting for state projection to restore remote tracks."));
        }, Qt::QueuedConnection);
    }

    void OnRoomMetadataChanged(const livekit::RoomInfo &room,
                               const std::string &oldMetadata,
                               const std::string &newMetadata) override {
        auto *coordinator = _coordinator;
        if (!coordinator || _generation == 0) return;
        const auto generation = _generation;
        const QString metadata = QString::fromStdString(newMetadata);
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, metadata]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            coordinator->parseRoomMetadata(metadata.toStdString());
        }, Qt::QueuedConnection);
    }

    void OnRoomUpdated(const livekit::RoomInfo &room) override {
        auto *coordinator = _coordinator;
        if (!coordinator || _generation == 0) return;
        const auto generation = _generation;
        const MeetingRoomInfo info = ToMeetingRoomInfo(room);
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, info]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            coordinator->_roomInfo = info;
            emit coordinator->roomInfoUpdated(info);
        }, Qt::QueuedConnection);
    }

    void OnConnectionQualityChanged(std::shared_ptr<livekit::Participant> participant,
                                    livekit::ConnectionQuality quality,
                                    float score) override {
        auto *coordinator = _coordinator;
        if (!participant || !coordinator || _generation == 0) return;
        const auto generation = _generation;
        const QString identity = QString::fromStdString(participant->identity());
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, identity, quality, score]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            const auto it = coordinator->_participants.find(identity);
            if (it == coordinator->_participants.end()) return;
            if (it->second.connectionQuality == quality &&
                it->second.connectionQualityScore == score) {
                return;
            }
            it->second.connectionQuality = quality;
            it->second.connectionQualityScore = score;
            coordinator->updateParticipantListAndNotify();
            emit coordinator->participantConnectionQualityChanged(
                identity, static_cast<int>(quality), score);
        }, Qt::QueuedConnection);
    }

    void OnTrackStreamStateChanged(
        std::shared_ptr<livekit::Participant> participant,
        std::shared_ptr<livekit::TrackPublication> publication,
        livekit::TrackPublication::StreamState state) override {
        auto *coordinator = _coordinator;
        if (!participant || !publication || !publication->track() ||
            !coordinator || _generation == 0) {
            return;
        }
        const auto generation = _generation;
        const QString identity = QString::fromStdString(participant->identity());
        const QString trackSid = QString::fromStdString(publication->sid());
        const bool isVideo = publication->track()->kind() == livekit::TrackKind::Video;
        const bool paused = state == livekit::TrackPublication::StreamState::Paused;

        // A participant may have multiple tracks of the same kind. Snapshot
        // their aggregate paused state on the native callback thread so the
        // Qt projection remains correct when one of them resumes.
        bool audioPaused = false;
        bool videoPaused = false;
        for (const auto &[sid, candidate] : participant->tracks()) {
            if (!candidate || !candidate->track() ||
                candidate->stream_state() != livekit::TrackPublication::StreamState::Paused) {
                continue;
            }
            if (candidate->track()->kind() == livekit::TrackKind::Audio) {
                audioPaused = true;
            } else if (candidate->track()->kind() == livekit::TrackKind::Video) {
                videoPaused = true;
            }
        }

        QMetaObject::invokeMethod(coordinator,
                                  [coordinator, generation, identity, trackSid, isVideo, paused,
                                   audioPaused, videoPaused]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            const auto it = coordinator->_participants.find(identity);
            if (it == coordinator->_participants.end()) return;
            const bool aggregate_changed =
                it->second.isAudioStreamPaused != audioPaused ||
                it->second.isVideoStreamPaused != videoPaused;
            if (aggregate_changed) {
                it->second.isAudioStreamPaused = audioPaused;
                it->second.isVideoStreamPaused = videoPaused;
                coordinator->updateParticipantListAndNotify();
            }
            // A second track of the same kind can change state while the
            // participant-level aggregate remains paused. Preserve that
            // track-level event for consumers such as a per-track renderer.
            emit coordinator->participantTrackStreamStateChanged(
                identity, trackSid, isVideo, paused);
        }, Qt::QueuedConnection);
    }

    void OnParticipantPermissionsChanged(
        const livekit::ParticipantPermission &oldPermission,
        const livekit::ParticipantPermission &newPermission,
        std::shared_ptr<livekit::Participant> participant) override {
        auto *coordinator = _coordinator;
        if (!participant || !coordinator || _generation == 0) return;
        const auto generation = _generation;
        const QString identity = QString::fromStdString(participant->identity());
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, identity, newPermission]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            const auto it = coordinator->_participants.find(identity);
            if (it == coordinator->_participants.end()) return;
            it->second.permissions = newPermission;
            coordinator->updateParticipantListAndNotify();
            emit coordinator->participantPermissionsChanged(identity, newPermission);
        }, Qt::QueuedConnection);
    }

    void OnTrackSubscriptionPermissionChanged(
        const livekit::TrackSubscriptionPermission &permission,
        std::shared_ptr<livekit::Participant> participant,
        std::shared_ptr<livekit::TrackPublication> publication) override {
        auto *coordinator = _coordinator;
        if (!coordinator || _generation == 0) return;
        const auto generation = _generation;
        const QString identity = participant
            ? QString::fromStdString(participant->identity())
            : QString();
        const QString participantSid = QString::fromStdString(permission.participant_sid);
        const QString trackSid = QString::fromStdString(permission.track_sid);
        const bool allowed = permission.allowed;
        QMetaObject::invokeMethod(coordinator,
                                  [coordinator, generation, identity, participantSid, trackSid, allowed]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            emit coordinator->trackSubscriptionPermissionChanged(
                identity, participantSid, trackSid, allowed);
        }, Qt::QueuedConnection);
    }

    void OnParticipantConnected(std::shared_ptr<livekit::RemoteParticipant> p) override {
        auto *coordinator = _coordinator;
        if (!p || !coordinator || _generation == 0) return;
        const auto generation = _generation;
        QString id = QString::fromStdString(p->identity());
        QString realNick = ResolveParticipantNickname(p);
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, id, realNick, p]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            MeetingUI::LogToConsole(MeetingUI::LogCategory::Participant, "REMOTE_JOIN", QCoreApplication::translate("MeetingUI", "Participant joined: %1 (display name: %2)").arg(id).arg(realNick));
            ParticipantInfo info;
            info.identity = id;
            info.name = realNick;
            info.isLocal = false;
            info.isHost = (id == coordinator->_meetingDetail.hostUserId || id == coordinator->_meetingDetail.creatorUserId);
            info.isAudioMuted = true;
            info.isVideoEnabled = false;
            CopyParticipantState(p, &info);
            coordinator->_participants[id] = info;
            coordinator->updateParticipantListAndNotify();
            emit coordinator->participantJoined(id, realNick);
        }, Qt::QueuedConnection);
    }

    void OnParticipantMetadataChanged(std::shared_ptr<livekit::Participant> p,
                                      const std::string &old_metadata,
                                      const std::string &new_metadata) override {
        auto *coordinator = _coordinator;
        if (!p || !coordinator || _generation == 0) return;
        const auto generation = _generation;
        QString id = QString::fromStdString(p->identity());
        QString realNick = ResolveParticipantNickname(p);
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, id, realNick]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            MeetingUI::LogToConsole(MeetingUI::LogCategory::Participant, "METADATA_CHANGED",
                                    QCoreApplication::translate("MeetingUI", "Participant [%1] metadata updated. Display name: %2").arg(id).arg(realNick));
            auto it = coordinator->_participants.find(id);
            if (it != coordinator->_participants.end()) {
                if (!it->second.isLocal) {
                    it->second.name = realNick;
                }
                coordinator->updateParticipantListAndNotify();
                emit coordinator->participantJoined(id, realNick);
            }
        }, Qt::QueuedConnection);
    }

    void OnParticipantDisconnected(std::shared_ptr<livekit::RemoteParticipant> p) override {
        auto *coordinator = _coordinator;
        if (!p || !coordinator || _generation == 0) return;
        const auto generation = _generation;
        QString id = QString::fromStdString(p->identity());
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, id]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            MeetingUI::LogToConsole(MeetingUI::LogCategory::Participant, "REMOTE_LEFT", QCoreApplication::translate("MeetingUI", "Participant left: %1").arg(id));
            coordinator->_participants.erase(id);
            coordinator->updateParticipantListAndNotify();
            emit coordinator->participantLeft(id);

        }, Qt::QueuedConnection);
    }

    void OnTrackSubscribed(std::shared_ptr<livekit::Track> track,
                           std::shared_ptr<livekit::TrackPublication> pub,
                           std::shared_ptr<livekit::RemoteParticipant> p) override {
        auto *coordinator = _coordinator;
        if (!track || !p || !coordinator || _generation == 0) return;
        const auto generation = _generation;
        std::string identity = p->identity();

        if (track->kind() == livekit::TrackKind::Video) {
            QMetaObject::invokeMethod(coordinator, [coordinator, generation, track, identity]() {
                if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
                emit coordinator->remoteVideoTrackAvailable(QString::fromStdString(identity), track);
            }, Qt::QueuedConnection);
        }
    }

    void OnTrackUnsubscribed(std::shared_ptr<livekit::Track> track,
                             std::shared_ptr<livekit::TrackPublication> pub,
                             std::shared_ptr<livekit::RemoteParticipant> p) override {
        auto *coordinator = _coordinator;
        if (!track || !p || !coordinator || _generation == 0) return;
        const auto generation = _generation;
        QString id = QString::fromStdString(p->identity());
        bool isVideo = (track->kind() == livekit::TrackKind::Video);
        const std::string track_sid = track->sid();
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, id, isVideo, track_sid]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            if (isVideo) {
                emit coordinator->remoteVideoTrackUnavailable(id, QString::fromStdString(track_sid));
            }
            auto it = coordinator->_participants.find(id);
            if (it != coordinator->_participants.end()) {
                if (isVideo) {
                    it->second.isVideoEnabled = false;
                } else {
                    it->second.isAudioMuted = true;
                }
                coordinator->updateParticipantListAndNotify();
            }
            emit coordinator->remoteTrackMuted(id, isVideo, true);
        }, Qt::QueuedConnection);
    }

    void OnTrackMuted(std::shared_ptr<livekit::Participant> participant,
                      std::shared_ptr<livekit::TrackPublication> publication,
                      bool muted) override {
        auto *coordinator = _coordinator;
        if (!participant || !publication || !publication->track() || !coordinator || _generation == 0) return;
        const auto generation = _generation;
        QString id = QString::fromStdString(participant->identity());
        bool isVideo = (publication->track()->kind() == livekit::TrackKind::Video);
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, id, isVideo, muted]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            auto it = coordinator->_participants.find(id);
            if (it != coordinator->_participants.end()) {
                if (isVideo) {
                    it->second.isVideoEnabled = !muted;
                } else {
                    it->second.isAudioMuted = muted;
                }
                coordinator->updateParticipantListAndNotify();
            }
            emit coordinator->remoteTrackMuted(id, isVideo, muted);
        }, Qt::QueuedConnection);
    }

    void OnActiveSpeakersChanged(const std::vector<std::shared_ptr<livekit::Participant>> &speakers) override {
        (void)speakers;
    }

    void OnDataReceived(const std::vector<uint8_t> &payload,
                        std::shared_ptr<livekit::RemoteParticipant> participant,
                        const std::string &topic) override {
        auto *coordinator = _coordinator;
        auto session = _session.lock();
        if (!coordinator || !session) return;
        (void)payload;
        (void)participant;
    }

    void OnTextStreamOpened(std::shared_ptr<livekit::TextStreamReader> reader,
                            std::shared_ptr<livekit::Participant> participant) override {
        auto *coordinator = _coordinator;
        auto session = _session.lock();
        if (!coordinator || !session || !reader) return;
        const auto generation = _generation;
        QString pId = participant ? QString::fromStdString(participant->identity()) : QString();
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, reader, pId]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            emit coordinator->textStreamReceived(reader, pId);
        }, Qt::QueuedConnection);
    }

    void OnByteStreamOpened(std::shared_ptr<livekit::ByteStreamReader> reader,
                            std::shared_ptr<livekit::Participant> participant) override {
        auto *coordinator = _coordinator;
        auto session = _session.lock();
        if (!coordinator || !session || !reader) return;
        const auto generation = _generation;
        QString pId = participant ? QString::fromStdString(participant->identity()) : QString();
        QMetaObject::invokeMethod(coordinator, [coordinator, generation, reader, pId]() {
            if (!coordinator->isCurrentSessionGenerationOnUiThread(generation)) return;
            emit coordinator->byteStreamReceived(reader, pId);
        }, Qt::QueuedConnection);
    }

private:
    MeetingCoordinator *_coordinator;
    std::weak_ptr<MeetingSessionRuntime> _session;
    const uint64_t _generation;
    const quint64 _authGeneration;
    const QString _localUserId;
};

bool MeetingCoordinator::applyAccountNotificationOnUiThread(uint64_t sessionGeneration, quint64 authGeneration,
        const QString &localUserId, const livekit::ParticipantEvent &event) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (event.kind != livekit::ParticipantEventKind::DataReceived ||
        event.sender.origin != livekit::SenderOrigin::Server) return false;
    openmeeting::meeting::NotifyMeetingData notify;
    if (!notify.ParseFromArray(event.data.data(), static_cast<int>(event.data.size())) ||
        !notify.has_kickoffmeetingdata() ||
        notify.kickoffmeetingdata().reasoncode() != openmeeting::meeting::KickOffReason::DuplicatedLogin) return false;

    // Account state belongs to this Qt owner. Do not send an accepted account
    // notification through Qt -> strand -> Qt: the server sends RemoveParticipant
    // immediately after it, which can retire the room before that round trip ends.
    if (!isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
        _sessionManager.authGeneration() != authGeneration || !_sessionManager.isLoggedIn() ||
        _sessionManager.userId() != localUserId ||
        QString::fromStdString(notify.kickoffmeetingdata().userid()) != localUserId) return true;
    QPointer<MeetingCoordinator> owner(this);
    MeetingUI::LogToConsole(MeetingUI::LogCategory::Connection, "DUPLICATED_LOGIN",
        "[Coordinator] Apply trusted server duplicated-login event");
    if (!owner || !owner->isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
        owner->_sessionManager.authGeneration() != authGeneration) return true;
    owner->_sessionManager.invalidateSession(SessionInvalidationReason::DuplicatedLogin);
    return true;
}

std::shared_ptr<livekit::RoomListener> MeetingCoordinator::participantEventListenerForTesting(
    const std::shared_ptr<MeetingSessionRuntime> &session, bool retainForOwnedSession) {
    auto listener = std::make_shared<CoordinatorRoomListener>(this, session);
    if (retainForOwnedSession) _roomListener = listener;
    return listener;
}

void MeetingCoordinator::applyParticipantEventOnUiThread(
    uint64_t coordinatorGeneration,
    const livekit::ParticipantEvent &event) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!isCurrentSessionGenerationOnUiThread(coordinatorGeneration)) return;
    // Every signal below may synchronously destroy the receiver or deliver a
    // replacement roster. Establish the weak owner before the first effect.
    QPointer<MeetingCoordinator> owner(this);
    const auto eventRoomGeneration = event.participant.key.native_room_generation;
    auto sessionStillCurrent = [&]() {
        return owner &&
            owner->isCurrentSessionGenerationOnUiThread(coordinatorGeneration) &&
            (eventRoomGeneration == 0 ||
             owner->_nativeRoomGeneration == eventRoomGeneration);
    };

    const bool participantCleanup =
        event.kind == livekit::ParticipantEventKind::Departure;
    const bool trackCleanup =
        event.kind == livekit::ParticipantEventKind::TrackUnavailable;
    const bool requiresParticipantTicket =
        event.kind != livekit::ParticipantEventKind::ActiveSpeakers &&
        event.kind != livekit::ParticipantEventKind::DataReceived &&
        !participantCleanup;

    if (requiresParticipantTicket &&
        !livekit::IsParticipantTicketActive(
            event.participant.ticket, event.participant.key)) {
        return;
    }

    if (!participantCleanup && !trackCleanup &&
        event.participant.key.native_room_generation != 0) {
        if (_nativeRoomGeneration != 0 &&
            eventRoomGeneration < _nativeRoomGeneration) {
            return;
        }
        if (_nativeRoomGeneration == 0 ||
            eventRoomGeneration > _nativeRoomGeneration) {
            std::vector<QString> oldRemoteIdentities;
            for (const auto &[identity, info] : _participants) {
                if (!info.isLocal) oldRemoteIdentities.push_back(identity);
                cancelInboundTransfersForParticipant(info.participantKey);
            }
            _nativeRoomGeneration = eventRoomGeneration;
            _participants.clear();
            _remoteVideoTracks.clear();
            _participantEventSequences.clear();
            updateParticipantListAndNotify();
            if (!sessionStillCurrent()) return;
            for (const auto &identity : oldRemoteIdentities) {
                // A reentrant roster may already have installed its successor.
                if (owner->_participants.find(identity) != owner->_participants.end()) continue;
                emit participantLeft(identity);
                if (!sessionStillCurrent()) return;
            }
        }
    }

    const auto key = event.participant.key;
    const QString identity = QString::fromStdString(
        key.identity.empty() ? key.sid : key.identity);
    const QString sequenceKey = ParticipantKeyToken(key);
    if (!sequenceKey.isEmpty() && key.incarnation != 0) {
        auto &last = _participantEventSequences[sequenceKey];
        if (event.event_sequence <= last) return;
        last = event.event_sequence;
    }

    auto participantStillCurrent = [&]() {
        if (!sessionStillCurrent()) return false;
        const auto it = owner->_participants.find(identity);
        return it != owner->_participants.end() &&
            it->second.participantKey == key;
    };
    auto valueStillCurrent = [&]() {
        if (!sessionStillCurrent() ||
            !livekit::IsParticipantTicketActive(event.participant.ticket, key)) return false;
        const auto sequence = owner->_participantEventSequences.find(sequenceKey);
        return sequence != owner->_participantEventSequences.end() &&
            sequence->second == event.event_sequence;
    };

    if (event.kind == livekit::ParticipantEventKind::Departure) {
        // Resource cancellation belongs to the retired key and must be queued
        // before any UI effect can replace the session or delete this owner.
        cancelInboundTransfersForParticipant(key);
        _participantEventSequences.erase(sequenceKey);
        std::vector<QString> trackSids;
        const auto tracks = _remoteVideoTracks.find(identity);
        if (tracks != _remoteVideoTracks.end()) {
            for (const auto &[sid, value] : tracks->second) {
                if (value.key.participant == key) trackSids.push_back(sid);
            }
        }
        if (participantStillCurrent()) {
            _participants.erase(identity);
            _remoteVideoTracks.erase(identity);
            updateParticipantListAndNotify();
            if (!sessionStillCurrent()) return;
            for (const auto &trackSid : trackSids) {
                if (owner->_participants.find(identity) != owner->_participants.end()) return;
                emit remoteVideoTrackUnavailable(identity, trackSid);
                if (!sessionStillCurrent()) return;
            }
            if (owner->_participants.find(identity) != owner->_participants.end()) return;
            emit participantLeft(identity);
        }
        return;
    }

    if (event.kind == livekit::ParticipantEventKind::ActiveSpeakers) {
        for (auto &[id, participant] : _participants) {
            participant.isSpeaking = false;
            participant.audioLevel = 0.0f;
        }
        std::vector<livekit::ActiveSpeakerInfo> accepted;
        for (const auto &speaker : event.speakers) {
            if (!livekit::IsParticipantTicketActive(speaker.ticket, speaker.key)) continue;
            const QString speakerIdentity = QString::fromStdString(
                speaker.key.identity.empty() ? speaker.key.sid : speaker.key.identity);
            const auto it = _participants.find(speakerIdentity);
            if (it == _participants.end() ||
                it->second.participantKey != speaker.key) {
                continue;
            }
            it->second.isSpeaking = speaker.speaking;
            it->second.audioLevel = speaker.audio_level;
            accepted.push_back(speaker);
        }
        updateParticipantListAndNotify();
        if (!sessionStillCurrent()) return;
        accepted.erase(std::remove_if(accepted.begin(), accepted.end(), [&](const auto &speaker) {
            if (!livekit::IsParticipantTicketActive(speaker.ticket, speaker.key)) return true;
            const QString id = QString::fromStdString(
                speaker.key.identity.empty() ? speaker.key.sid : speaker.key.identity);
            const auto current = owner->_participants.find(id);
            return current == owner->_participants.end() ||
                current->second.participantKey != speaker.key;
        }), accepted.end());
        emit activeSpeakersChanged(accepted);
        return;
    }

    if (event.kind == livekit::ParticipantEventKind::DataReceived) {
        const bool serverOrigin = event.sender.origin == livekit::SenderOrigin::Server;
        if (!serverOrigin &&
            !livekit::IsParticipantTicketActive(
                event.sender.ticket, event.sender.key)) {
            return;
        }
        auto session = _sessionRuntime;
        if (session) enqueueDataReceived(session, event.data, event.sender);
        return;
    }

    if (event.kind == livekit::ParticipantEventKind::TextStreamOpened ||
        event.kind == livekit::ParticipantEventKind::ByteStreamOpened) {
        if (!livekit::IsParticipantTicketActive(
                event.sender.ticket, event.sender.key)) {
            return;
        }
        const QString senderIdentity = QString::fromStdString(
            event.sender.key.identity.empty()
                ? event.sender.key.sid
                : event.sender.key.identity);
        if (event.kind == livekit::ParticipantEventKind::TextStreamOpened &&
            event.text_reader) {
            emit textStreamReceived(event.text_reader, senderIdentity);
        } else if (event.byte_reader) {
            emit byteStreamReceived(event.byte_reader, senderIdentity);
        }
        return;
    }

    if (event.kind == livekit::ParticipantEventKind::TrackUnavailable) {
        const QString trackSid = QString::fromStdString(
            event.track_key.publication_sid);
        auto participantTracks = _remoteVideoTracks.find(identity);
        if (participantTracks == _remoteVideoTracks.end()) return;
        const auto track = participantTracks->second.find(trackSid);
        if (track == participantTracks->second.end() ||
            track->second.key != event.track_key ||
            (event.media_binding_key.serial != 0 &&
             track->second.mediaBindingKey != event.media_binding_key)) {
            return;
        }
        participantTracks->second.erase(track);
        if (participantTracks->second.empty()) {
            _remoteVideoTracks.erase(participantTracks);
        }
        emit remoteVideoTrackUnavailable(identity, trackSid);
        return;
    }

    if (!livekit::IsParticipantTicketActive(
            event.participant.ticket, key)) {
        return;
    }

    if (event.kind == livekit::ParticipantEventKind::Upsert ||
        event.kind == livekit::ParticipantEventKind::ConnectionQuality ||
        event.kind == livekit::ParticipantEventKind::TrackMuted ||
        event.kind == livekit::ParticipantEventKind::TrackStreamState ||
        event.kind == livekit::ParticipantEventKind::TrackSubscriptionPermission) {
        const auto existing = _participants.find(identity);
        const bool replacing = existing != _participants.end() &&
            existing->second.participantKey != key;
        const bool isNew = existing == _participants.end() || replacing;
        if (replacing) {
            const auto oldKey = existing->second.participantKey;
            _participants.erase(existing);
            _remoteVideoTracks.erase(identity);
            cancelInboundTransfersForParticipant(oldKey);
            updateParticipantListAndNotify();
            if (!valueStillCurrent() || owner->_participants.find(identity) != owner->_participants.end()) return;
            emit participantLeft(identity);
            if (!valueStillCurrent() || owner->_participants.find(identity) != owner->_participants.end()) return;
        }

        ParticipantInfo info;
        if (const auto current = _participants.find(identity);
            current != _participants.end()) {
            info = current->second;
        }
        info.identity = identity;
        info.name = ResolveParticipantNickname(event.participant.state);
        info.isLocal = event.participant.is_local;
        info.isHost = identity == _meetingDetail.hostUserId ||
            identity == _meetingDetail.creatorUserId;
        info.isAudioMuted = info.isLocal ? _audioMuted : true;
        info.isVideoEnabled = info.isLocal ? _videoEnabled : false;
        info.isSpeaking = event.participant.state.speaking;
        info.audioLevel = event.participant.state.audio_level;
        info.participantKey = key;
        info.participantTicket = event.participant.ticket;
        info.lastEventSequence = event.event_sequence;
        CopyParticipantState(event.participant.state, &info);
        // Local effective state includes device readiness and newer UI intent.
        // An older publication snapshot must not re-enable a failed device.
        for (const auto &publication : event.participant.state.publications) {
            if (!publication.track) continue;
            if (!info.isLocal && publication.kind == livekit::TrackKind::Audio) {
                info.isAudioMuted = publication.muted;
            } else if (publication.kind == livekit::TrackKind::Video) {
                if (!info.isLocal) info.isVideoEnabled = !publication.muted;
                const auto participantTracks = _remoteVideoTracks.find(identity);
                if (participantTracks != _remoteVideoTracks.end()) {
                    const auto video = participantTracks->second.find(QString::fromStdString(publication.sid));
                    if (video != participantTracks->second.end() &&
                        video->second.key.participant == key && video->second.track == publication.track) {
                        video->second.muted = publication.muted;
                        video->second.paused = publication.stream_state == livekit::TrackPublication::StreamState::Paused;
                    }
                }
            }
        }
        _participants[identity] = info;
        updateParticipantListAndNotify();
        if (!valueStillCurrent() || !participantStillCurrent()) return;

        if (isNew && !info.isLocal) {
            emit participantJoined(identity, info.name);
            if (!valueStillCurrent() || !participantStillCurrent()) return;
        }

        if (event.kind == livekit::ParticipantEventKind::ConnectionQuality) {
            emit participantConnectionQualityChanged(
                identity,
                static_cast<int>(info.connectionQuality),
                info.connectionQualityScore);
        } else if (event.kind == livekit::ParticipantEventKind::TrackMuted) {
            emit remoteTrackMuted(
                identity,
                event.publication.kind == livekit::TrackKind::Video,
                event.publication.muted);
        } else if (event.kind == livekit::ParticipantEventKind::TrackStreamState) {
            emit participantTrackStreamStateChanged(
                identity,
                QString::fromStdString(event.track_key.publication_sid),
                event.publication.kind == livekit::TrackKind::Video,
                event.publication.stream_state ==
                    livekit::TrackPublication::StreamState::Paused);
        } else if (event.kind == livekit::ParticipantEventKind::TrackSubscriptionPermission) {
            emit trackSubscriptionPermissionChanged(
                identity,
                QString::fromStdString(key.sid),
                QString::fromStdString(event.track_key.publication_sid),
                event.publication.subscription_allowed);
        }
    }

    if (event.kind == livekit::ParticipantEventKind::TrackAvailable) {
        if (!participantStillCurrent() ||
            !livekit::IsTrackTicketActive(
                event.track_ticket, event.track_key) ||
            !livekit::IsMediaBindingTicketActive(
                event.media_binding_ticket, event.media_binding_key) ||
            event.publication.kind != livekit::TrackKind::Video ||
            !event.publication.track) {
            return;
        }
        const QString trackSid = QString::fromStdString(
            event.track_key.publication_sid);
        const auto participantTracks = _remoteVideoTracks.find(identity);
        if (participantTracks != _remoteVideoTracks.end()) {
            const auto existing = participantTracks->second.find(trackSid);
            if (existing != participantTracks->second.end() &&
                existing->second.key == event.track_key &&
                existing->second.mediaBindingKey == event.media_binding_key &&
                existing->second.track == event.publication.track) {
                return;
            }
        }
        _remoteVideoTracks[identity][trackSid] = {
            event.track_key,
            event.track_ticket,
            event.media_binding_key,
            event.media_binding_ticket,
            event.publication.track,
            event.publication.muted,
            event.publication.stream_state == livekit::TrackPublication::StreamState::Paused};
        emit remoteVideoTrackAvailable(identity, event.publication.track);
    }
}

// ----------------------------------------------------
// MeetingCoordinator 实现
// ----------------------------------------------------

std::shared_ptr<MeetingCoordinator> MeetingCoordinator::create(QObject *parent) {
    return std::make_shared<MeetingCoordinator>(parent);
}

MeetingCoordinator::MeetingCoordinator(QObject *parent)
    : MeetingCoordinator(SessionManager::instance(),
                         makeDefaultAdmissionBackend(SessionManager::instance()),
                         parent) {
}

MeetingCoordinator::AdmissionBackend MeetingCoordinator::makeDefaultAdmissionBackend(
    SessionManager &sessionManager) {
    AdmissionBackend backend;
    backend.joinMeeting = [&sessionManager](const QString &meetingId,
                                            const QString &password,
                                            ResultCallback<bool> callback) {
        sessionManager.httpClient().joinMeeting(meetingId, password, std::move(callback));
    };
    backend.getMeetingToken = [&sessionManager](const QString &meetingId,
                                                ResultCallback<LiveKitAuthInfo> callback) {
        sessionManager.httpClient().getMeetingToken(meetingId, std::move(callback));
    };
    backend.createImmediateMeeting = [&sessionManager](const QString &title,
                                                       int durationSeconds,
                                                       ResultCallback<LiveKitAuthInfo> callback) {
        sessionManager.httpClient().createImmediateMeeting(title, durationSeconds, std::move(callback));
    };
    backend.leaveMeeting = [&sessionManager](const QString &meetingId,
                                             ResultCallback<bool> callback) {
        sessionManager.httpClient().leaveMeeting(meetingId, std::move(callback));
    };
    backend.endMeeting = [&sessionManager](const QString &meetingId,
                                           ResultCallback<bool> callback) {
        sessionManager.httpClient().endMeeting(meetingId, std::move(callback));
    };
    return backend;
}

MeetingCoordinator::MeetingCoordinator(SessionManager &sessionManager,
                                       AdmissionBackend admissionBackend,
                                       QObject *parent)
    : QObject(parent)
    , _sessionManager(sessionManager)
    , _admissionBackend(std::move(admissionBackend)) {
    qRegisterMetaType<livekit::RoomDisconnectReason>("livekit::RoomDisconnectReason");
    qRegisterMetaType<MeetingRoomInfo>("OpenMeeting::MeetingRoomInfo");
    qRegisterMetaType<livekit::ParticipantPermission>("livekit::ParticipantPermission");
    qRegisterMetaType<std::shared_ptr<livekit::TextStreamReader>>("std::shared_ptr<livekit::TextStreamReader>");
    qRegisterMetaType<std::shared_ptr<livekit::ByteStreamReader>>("std::shared_ptr<livekit::ByteStreamReader>");
    _localAudioSource = std::make_shared<livekit::AudioSource>(48000, 2);
    _localVideoSource = std::make_shared<livekit::VideoSource>(1280, 720);

    _mediaSendTimer = new QTimer(this);
    connect(_mediaSendTimer, &QTimer::timeout, this, &MeetingCoordinator::processNextMediaSendChunk);
    _whiteboardTickTimer = new QTimer(this);
    // Asset packets are pumped in small batches. A short tick keeps large image
    // pages responsive while heartbeat/retry work remains time-gated in Runtime.
    _whiteboardTickTimer->setInterval(50);
    connect(_whiteboardTickTimer, &QTimer::timeout, this, [this] {
        auto session = _sessionRuntime;
        if (!session || !_sessionRunning.load(std::memory_order_acquire)) return;
        const auto now = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
        asio::post(session->strand(), [session, now] {
            if (auto board = session->whiteboardOnStrand()) board->tick(now);
        });
    });

    // 全局账号状态由 SessionManager 统一裁决。这里不发 meetingLeft，避免
    // MeetingRoomWindow 按普通离会逻辑继续调用业务 HTTP 接口。
    connect(&_sessionManager, &SessionManager::sessionInvalidated,
            this, [this](SessionInvalidationReason reason) {
                handleSessionInvalidated(reason);
            }, Qt::QueuedConnection);
    connect(&_sessionManager, &SessionManager::loggedOut, this, [this]() {
        if (!_sessionManager.isSessionInvalidating())
            handleSessionInvalidated(SessionInvalidationReason::UserLogout);
    });
}

MeetingCoordinator::~MeetingCoordinator() {
    invalidateAdmission();
    stopRoomSession();
}

QString MeetingCoordinator::stateString() const {
    switch (_state) {
        case MeetingState::Idle: return QCoreApplication::translate("MeetingUI", "Not Ready / Idle");
        case MeetingState::Validating: return QCoreApplication::translate("MeetingUI", "Checking Access");
        case MeetingState::FetchingCredentials: return QCoreApplication::translate("MeetingUI", "Fetching Credentials");
        case MeetingState::ConnectingRoom: return QCoreApplication::translate("MeetingUI", "Connecting to Room");
        case MeetingState::StartingLocalMedia: return QCoreApplication::translate("MeetingUI", "Starting Local Media");
        case MeetingState::InMeeting: return QCoreApplication::translate("MeetingUI", "In Meeting");
        case MeetingState::Reconnecting: return QCoreApplication::translate("MeetingUI", "Reconnecting");
        case MeetingState::Leaving: return QCoreApplication::translate("MeetingUI", "Leaving and Cleaning Up");
        case MeetingState::Failed: return QCoreApplication::translate("MeetingUI", "Operation Failed");
    }
    return QCoreApplication::translate("MeetingUI", "Unknown State");
}

bool MeetingCoordinator::isHost() const {
    QString myUserId = _sessionManager.userId();
    if (myUserId.isEmpty()) return false;
    return (myUserId == _meetingDetail.hostUserId || myUserId == _meetingDetail.creatorUserId);
}

uint64_t MeetingCoordinator::beginAdmission(AdmissionStage stage) {
    if (!_admissionTelemetry.terminal) {
        finishAdmissionTelemetry(
            _admissionTelemetry.generation,
            livekit::telemetry::OperationOutcome::Cancelled);
    }
    ++_admissionGeneration;
    _admissionStage = stage;
    _admissionTelemetry = AdmissionTelemetryRecord{};
    _admissionTelemetry.generation = _admissionGeneration;
    _admissionTelemetry.startedAt = std::chrono::steady_clock::now();
    _admissionTelemetry.terminal = false;
    _admissionTelemetry.stabilityLedger =
        livekit::telemetry::InstalledStabilityLedger();
    if (_admissionTelemetry.stabilityLedger) {
        _admissionTelemetry.stabilitySessionId =
            _admissionTelemetry.stabilityLedger->BeginSession();
    }
    return _admissionGeneration;
}

uint64_t MeetingCoordinator::invalidateAdmission() {
    if (!_admissionTelemetry.terminal) {
        finishAdmissionTelemetry(
            _admissionTelemetry.generation,
            livekit::telemetry::OperationOutcome::Cancelled);
    }
    ++_admissionGeneration;
    _admissionStage = AdmissionStage::None;
    return _admissionGeneration;
}

void MeetingCoordinator::attachAdmissionTelemetry(
    const std::shared_ptr<livekit::telemetry::SessionTelemetry> &telemetry) {
    if (!telemetry || _admissionTelemetry.terminal ||
        _admissionTelemetry.generation != _admissionGeneration) {
        return;
    }
    _admissionTelemetry.telemetry = telemetry;
    _admissionTelemetry.operationId = telemetry->StartOperation(
        livekit::telemetry::OperationKind::Admission,
        "admission",
        _admissionTelemetry.startedAt);
}

void MeetingCoordinator::finishAdmissionTelemetry(
    uint64_t admissionGeneration,
    livekit::telemetry::OperationOutcome outcome) {
    if (_admissionTelemetry.terminal ||
        _admissionTelemetry.generation != admissionGeneration) {
        return;
    }
    const auto finishedAt = std::chrono::steady_clock::now();
    _admissionTelemetry.terminal = true;
    if (const auto telemetry = _admissionTelemetry.telemetry.lock();
        telemetry && !_admissionTelemetry.operationId.empty()) {
        telemetry->FinishOperation(
            _admissionTelemetry.operationId,
            livekit::telemetry::OperationKind::Admission,
            outcome,
            finishedAt);
    } else {
        publishDetachedAdmissionTelemetry(outcome, finishedAt);
    }
    auto stability = _admissionTelemetry.stabilityLedger;
    auto stabilitySessionId = std::move(
        _admissionTelemetry.stabilitySessionId);
    _admissionTelemetry.stabilityLedger.reset();
    if (stability && !stabilitySessionId.empty()) {
        if (outcome == livekit::telemetry::OperationOutcome::Success ||
            outcome == livekit::telemetry::OperationOutcome::DegradedSuccess) {
            if (!_activeStabilitySessionId.empty()) {
                finishActiveStabilitySession();
            }
            _activeStabilityLedger = std::move(stability);
            _activeStabilitySessionId = std::move(stabilitySessionId);
        } else {
            using Terminal = livekit::telemetry::StabilitySessionTerminal;
            const auto terminal = outcome == livekit::telemetry::OperationOutcome::Timeout
                ? Terminal::Timeout
                : outcome == livekit::telemetry::OperationOutcome::Cancelled
                    ? Terminal::Cancelled
                    : Terminal::AdmissionFailure;
            stability->FinishSession(stabilitySessionId, terminal);
        }
    }
}

void MeetingCoordinator::finishActiveStabilitySession() {
    if (_activeStabilityLedger && !_activeStabilitySessionId.empty()) {
        _activeStabilityLedger->FinishSession(
            _activeStabilitySessionId,
            livekit::telemetry::StabilitySessionTerminal::Stopped);
    }
    _activeStabilitySessionId.clear();
    _activeStabilityLedger.reset();
}

void MeetingCoordinator::finishStartupTelemetry(
    livekit::telemetry::OperationOutcome outcome) {
    if (_startupTelemetryOperationId.empty()) return;
    if (const auto telemetry = _startupTelemetry.lock()) {
        telemetry->FinishOperation(
            _startupTelemetryOperationId,
            livekit::telemetry::OperationKind::Startup,
            outcome);
    }
    _startupTelemetryOperationId.clear();
    _startupTelemetry.reset();
}

void MeetingCoordinator::publishDetachedAdmissionTelemetry(
    livekit::telemetry::OperationOutcome outcome,
    std::chrono::steady_clock::time_point finishedAt) {
    const auto durationMs = (std::max)(qint64{0},
        static_cast<qint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
            finishedAt - _admissionTelemetry.startedAt).count()));
    QVariantMap summary;
    summary.insert(QStringLiteral("kind"), QStringLiteral("admission"));
    summary.insert(QStringLiteral("started"), QVariant::fromValue<qulonglong>(1));
    summary.insert(QStringLiteral("terminal"), QVariant::fromValue<qulonglong>(1));
    summary.insert(QStringLiteral("success"), QVariant::fromValue<qulonglong>(
        outcome == livekit::telemetry::OperationOutcome::Success ? 1 : 0));
    summary.insert(QStringLiteral("degradedSuccess"), QVariant::fromValue<qulonglong>(
        outcome == livekit::telemetry::OperationOutcome::DegradedSuccess ? 1 : 0));
    summary.insert(QStringLiteral("failure"), QVariant::fromValue<qulonglong>(
        outcome == livekit::telemetry::OperationOutcome::Failure ? 1 : 0));
    summary.insert(QStringLiteral("timeout"), QVariant::fromValue<qulonglong>(
        outcome == livekit::telemetry::OperationOutcome::Timeout ? 1 : 0));
    summary.insert(QStringLiteral("cancelled"), QVariant::fromValue<qulonglong>(
        outcome == livekit::telemetry::OperationOutcome::Cancelled ? 1 : 0));
    summary.insert(QStringLiteral("inflight"), QVariant::fromValue<qulonglong>(0));
    summary.insert(QStringLiteral("lastDurationMs"), durationMs);

    QVariantMap projection;
    projection.insert(QStringLiteral("sessionGeneration"),
                      QVariant::fromValue<qulonglong>(0));
    projection.insert(QStringLiteral("revision"),
                      QVariant::fromValue<qulonglong>(++_detachedTelemetryRevision));
    projection.insert(QStringLiteral("availability"), QStringLiteral("UNKNOWN"));
    projection.insert(QStringLiteral("reason"), QStringLiteral("admission_pre_room_terminal"));
    projection.insert(QStringLiteral("sampleAgeMs"), -1);
    projection.insert(QStringLiteral("coverage"), 0.0);
    projection.insert(QStringLiteral("operationsStarted"),
                      QVariant::fromValue<qulonglong>(1));
    projection.insert(QStringLiteral("operationsTerminal"),
                      QVariant::fromValue<qulonglong>(1));
    projection.insert(QStringLiteral("operationsInflight"),
                      QVariant::fromValue<qulonglong>(0));
    projection.insert(QStringLiteral("operationsMissingStart"),
                      QVariant::fromValue<qulonglong>(0));
    projection.insert(QStringLiteral("operationsDuplicateTerminal"),
                      QVariant::fromValue<qulonglong>(0));
    projection.insert(QStringLiteral("operationsKindMismatch"),
                      QVariant::fromValue<qulonglong>(0));
    projection.insert(QStringLiteral("operationSummaries"),
                      QVariantList{summary});
    emit telemetrySnapshotChanged(projection);
}

bool MeetingCoordinator::canBeginAdmission() const {
    return _admissionStage == AdmissionStage::None ||
           _admissionStage == AdmissionStage::Consumed;
}

bool MeetingCoordinator::isAdmissionCurrent(uint64_t generation,
                                            AdmissionStage stage) const {
    return !_sessionInvalidated && !_sessionManager.isSessionInvalidating() &&
           _admissionGeneration == generation &&
           _admissionStage == stage;
}

void MeetingCoordinator::setState(MeetingState s, const QString &detail) {
    if (_state != s) {
        _state = s;
        emit stateChanged(_state, detail);
    }
}

void MeetingCoordinator::joinMeetingAsync(const QString &meetingId,
                                         const QString &password,
                                         const QString &displayName,
                                         const MediaPreferences &prefs) {
    if (_sessionInvalidated || _sessionManager.isSessionInvalidating()) {
        qInfo() << "[Coordinator] Ignore join request after session invalidation.";
        return;
    }
    if ((_state != MeetingState::Idle && _state != MeetingState::Failed) ||
        !canBeginAdmission()) {
        emit errorOccurred(QCoreApplication::translate("MeetingUI", "Join Error"), QCoreApplication::translate("MeetingUI", "A meeting operation is already in progress. Do not join again."));
        return;
    }

    const QString requestedMeetingId = meetingId;
    const QString requestedPassword = password;
    const QString requestedDisplayName = displayName;
    const uint64_t generation = beginAdmission(AdmissionStage::Joining);
    QPointer<MeetingCoordinator> owner(this);

    _currentMeetingId = requestedMeetingId;
    _currentPassword = requestedPassword;
    _currentDisplayName = requestedDisplayName.isEmpty() ? _sessionManager.nickname() : requestedDisplayName;
    _mediaPrefs = prefs;
    _requestedAudioMuted = !prefs.enableMicrophone;
    _requestedVideoEnabled = prefs.enableVideo;
    _audioMuted = _requestedAudioMuted || !_localAudioAvailable;
    _videoEnabled = _requestedVideoEnabled && _localVideoAvailable;

    _participants.clear();
    ensureLocalParticipant();
    updateParticipantListAndNotify();
    if (!owner || !owner->isAdmissionCurrent(generation, AdmissionStage::Joining)) {
        return;
    }

    owner->setState(MeetingState::Validating, QCoreApplication::translate("MeetingUI", "Checking meeting access..."));
    if (!owner || !owner->isAdmissionCurrent(generation, AdmissionStage::Joining)) {
        return;
    }

    // 第一阶段：向后端鉴权校验密码与会议有效性
    auto joinRequest = owner->_admissionBackend.joinMeeting;
    joinRequest(requestedMeetingId, requestedPassword,
                [owner, generation, requestedMeetingId](bool ok, bool, const HttpError &err) {
        if (!owner || !owner->isAdmissionCurrent(generation, AdmissionStage::Joining)) {
            return;
        }
        if (!ok) {
            const QString detail = err.message;
            const QString message = detail.isEmpty()
                ? QCoreApplication::translate("MeetingUI", "The meeting does not exist or the password is incorrect") : detail;
            owner->finishAdmissionTelemetry(
                generation, livekit::telemetry::OperationOutcome::Failure);
            owner->_admissionStage = AdmissionStage::None;
            owner->setState(MeetingState::Failed, detail);
            if (!owner || owner->_admissionGeneration != generation ||
                owner->_admissionStage != AdmissionStage::None) {
                return;
            }
            emit owner->errorOccurred(QCoreApplication::translate("MeetingUI", "Meeting Authentication Failed"), message);
            return;
        }

        // 第二阶段：换取 LiveKit 令牌与网关 URL
        owner->_admissionStage = AdmissionStage::FetchingToken;
        owner->setState(MeetingState::FetchingCredentials,
                        QCoreApplication::translate("MeetingUI", "Requesting an audio/video session token..."));
        if (!owner || !owner->isAdmissionCurrent(generation, AdmissionStage::FetchingToken)) {
            return;
        }
        auto tokenRequest = owner->_admissionBackend.getMeetingToken;
        tokenRequest(requestedMeetingId, [owner, generation](bool tokenOk,
                                                            const LiveKitAuthInfo &auth,
                                                            const HttpError &tokenErr) {
            if (!owner || !owner->isAdmissionCurrent(generation, AdmissionStage::FetchingToken)) {
                return;
            }
            if (!tokenOk || auth.url.isEmpty() || auth.token.isEmpty()) {
                const QString detail = tokenErr.message;
                const QString message = detail.isEmpty()
                    ? QCoreApplication::translate("MeetingUI", "Unable to obtain LiveKit room credentials") : detail;
                owner->finishAdmissionTelemetry(
                    generation, livekit::telemetry::OperationOutcome::Failure);
                owner->_admissionStage = AdmissionStage::None;
                owner->setState(MeetingState::Failed, detail);
                if (!owner || owner->_admissionGeneration != generation ||
                    owner->_admissionStage != AdmissionStage::None) {
                    return;
                }
                emit owner->errorOccurred(QCoreApplication::translate("MeetingUI", "Unable to Obtain Credentials"), message);
                return;
            }

            // 第三阶段：启动 LiveKit 房间连接与媒体发布
            const QString url = auth.url;
            const QString token = auth.token;
            owner->_admissionStage = AdmissionStage::ReadyToStart;
            owner->startRoomSession(url, token, generation);
        });
    });
}

void MeetingCoordinator::createAndJoinQuickMeetingAsync(const QString &title,
                                                       int durationSeconds,
                                                       const MediaPreferences &prefs) {
    if (_sessionInvalidated || _sessionManager.isSessionInvalidating()) {
        qInfo() << "[Coordinator] Ignore quick-meeting request after session invalidation.";
        return;
    }
    if ((_state != MeetingState::Idle && _state != MeetingState::Failed) ||
        !canBeginAdmission()) {
        emit errorOccurred(QCoreApplication::translate("MeetingUI", "Create Error"), QCoreApplication::translate("MeetingUI", "A meeting operation is already active"));
        return;
    }

    const QString requestedTitle = title;
    const int requestedDurationSeconds = durationSeconds;
    const uint64_t generation = beginAdmission(AdmissionStage::Creating);
    QPointer<MeetingCoordinator> owner(this);

    _currentDisplayName = _sessionManager.nickname();
    _mediaPrefs = prefs;
    _requestedAudioMuted = !prefs.enableMicrophone;
    _requestedVideoEnabled = prefs.enableVideo;
    _audioMuted = _requestedAudioMuted || !_localAudioAvailable;
    _videoEnabled = _requestedVideoEnabled && _localVideoAvailable;

    _participants.clear();
    ensureLocalParticipant();
    updateParticipantListAndNotify();
    if (!owner || !owner->isAdmissionCurrent(generation, AdmissionStage::Creating)) {
        return;
    }

    owner->setState(MeetingState::Validating, QCoreApplication::translate("MeetingUI", "Creating an instant meeting..."));
    if (!owner || !owner->isAdmissionCurrent(generation, AdmissionStage::Creating)) {
        return;
    }

    auto createRequest = owner->_admissionBackend.createImmediateMeeting;
    createRequest(requestedTitle, requestedDurationSeconds,
                  [owner, generation, requestedTitle](bool ok,
                                                      const LiveKitAuthInfo &auth,
                                                      const HttpError &err) {
        if (!owner || !owner->isAdmissionCurrent(generation, AdmissionStage::Creating)) {
            return;
        }
        if (!ok || auth.url.isEmpty() || auth.token.isEmpty()) {
            const QString detail = err.message;
            const QString message = detail.isEmpty()
                ? QCoreApplication::translate("MeetingUI", "The server could not allocate a meeting room") : detail;
            owner->finishAdmissionTelemetry(
                generation, livekit::telemetry::OperationOutcome::Failure);
            owner->_admissionStage = AdmissionStage::None;
            owner->setState(MeetingState::Failed, detail);
            if (!owner || owner->_admissionGeneration != generation ||
                owner->_admissionStage != AdmissionStage::None) {
                return;
            }
            emit owner->errorOccurred(QCoreApplication::translate("MeetingUI", "Unable to Create Instant Meeting"), message);
            return;
        }

        const QString meetingId = auth.meetingId;
        const QString url = auth.url;
        const QString token = auth.token;
        const QString userId = owner->_sessionManager.userId();
        owner->_admissionStage = AdmissionStage::ReadyToStart;
        owner->_currentMeetingId = meetingId;
        owner->_meetingDetail.meetingId = meetingId;
        owner->_meetingDetail.meetingName = requestedTitle;
        owner->_meetingDetail.hostUserId = userId;
        owner->_meetingDetail.creatorUserId = userId;
        const MeetingDetail detailSnapshot = owner->_meetingDetail;
        emit owner->meetingDetailUpdated(detailSnapshot);
        if (!owner || !owner->isAdmissionCurrent(generation, AdmissionStage::ReadyToStart)) {
            return;
        }

        MeetingUI::LogToConsole(MeetingUI::LogCategory::General, "MEETING_ID",
            QCoreApplication::translate("MeetingUI", "Instant meeting created! Meeting ID: %1 (others can join using this 9-digit ID)").arg(meetingId));

        owner->startRoomSession(url, token, generation);
    });
}

void MeetingCoordinator::connectDirectlyAsync(const QString &url,
                                             const QString &token,
                                             const QString &meetingId,
                                             const QString &displayName,
                                             const MediaPreferences &prefs) {
    invalidateAdmission();
    if (_sessionInvalidated || _sessionManager.isSessionInvalidating()) {
        qInfo() << "[Coordinator] Ignore direct-connect request after session invalidation.";
        return;
    }
    const QString requestedUrl = url;
    const QString requestedToken = token;
    const uint64_t generation = beginAdmission(AdmissionStage::ReadyToStart);
    QPointer<MeetingCoordinator> owner(this);
    _currentMeetingId = meetingId;
    _currentDisplayName = displayName;
    _mediaPrefs = prefs;
    _requestedAudioMuted = !prefs.enableMicrophone;
    _requestedVideoEnabled = prefs.enableVideo;
    _audioMuted = _requestedAudioMuted || !_localAudioAvailable;
    _videoEnabled = _requestedVideoEnabled && _localVideoAvailable;

    _participants.clear();
    ensureLocalParticipant();
    updateParticipantListAndNotify();
    if (!owner || !owner->isAdmissionCurrent(generation, AdmissionStage::ReadyToStart)) {
        return;
    }

    owner->startRoomSession(requestedUrl, requestedToken, generation);
}

void MeetingCoordinator::leaveMeetingAsync(bool endMeetingForAll) {
    invalidateAdmission();
    if (_state == MeetingState::Idle || _state == MeetingState::Leaving) {
        return;
    }

    const QString meetingId = _currentMeetingId;
    const bool notifyBackend = !meetingId.isEmpty() && _sessionManager.isLoggedIn();
    const bool shouldEndMeeting = endMeetingForAll && isHost();
    auto backendRequest = shouldEndMeeting
        ? _admissionBackend.endMeeting : _admissionBackend.leaveMeeting;
    QPointer<MeetingCoordinator> owner(this);

    setState(MeetingState::Leaving, QCoreApplication::translate("MeetingUI", "Leaving the meeting safely..."));
    if (!owner || owner->_state != MeetingState::Leaving ||
        owner->_admissionStage != AdmissionStage::None || owner->_sessionInvalidated) {
        return;
    }
    if (notifyBackend) {
        backendRequest(meetingId, [](bool, bool, const HttpError &) {});
        if (!owner || owner->_state != MeetingState::Leaving ||
            owner->_admissionStage != AdmissionStage::None || owner->_sessionInvalidated) {
            return;
        }
    }

    owner->stopRoomSession();
    if (!owner || owner->_state != MeetingState::Leaving ||
        owner->_admissionStage != AdmissionStage::None || owner->_sessionInvalidated) {
        return;
    }

    owner->setState(MeetingState::Idle, QCoreApplication::translate("MeetingUI", "Left Meeting"));
    if (!owner || owner->_state != MeetingState::Idle ||
        owner->_admissionStage != AdmissionStage::None || owner->_sessionInvalidated) {
        return;
    }
    emit owner->meetingLeft();
}

void MeetingCoordinator::handleDuplicateIdentityKickOff(const QString &detail) {
    const uint64_t generation = invalidateAdmission();
    if (_state == MeetingState::Leaving || _state == MeetingState::Idle) {
        return;
    }
    QPointer<MeetingCoordinator> owner(this);
    const auto sessionGeneration = _nextSessionGeneration;

    const QString message = detail.isEmpty()
        ? QCoreApplication::translate("MeetingUI", "This account joined the meeting on another device")
        : QString::fromStdString(
            livekit::secure_log::SanitizeForOutput(detail.toStdString()));
    MeetingUI::LogToConsole(MeetingUI::LogCategory::Connection,
                            "DUPLICATE_IDENTITY",
                            QString("[Coordinator] Meeting kicked off by server: %1").arg(message));
    // Logging can replace the session or destroy its QObject. Admission
    // invalidation alone must not suppress cleanup of the original resources.
    if (!owner || owner->_nextSessionGeneration != sessionGeneration) return;

    // Room 已由服务端 LEAVE 流程断开；这里负责停止 Coordinator 所属的
    // io 线程和媒体资源。不要发出 meetingLeft，否则 UI 会在提示前关闭。
    owner->stopRoomSession();
    if (!owner || owner->_admissionGeneration != generation ||
        owner->_admissionStage != AdmissionStage::None) {
        return;
    }
    owner->setState(MeetingState::Idle, message);
    if (!owner || owner->_admissionGeneration != generation ||
        owner->_admissionStage != AdmissionStage::None || owner->_state != MeetingState::Idle) {
        return;
    }
    emit owner->meetingKickOff(livekit::RoomDisconnectReason::DuplicateIdentity);
}

void MeetingCoordinator::handleSessionInvalidated(SessionInvalidationReason reason) {
    invalidateAdmission();
    if (_sessionInvalidated) {
        return;
    }
    _sessionInvalidated = true;

    MeetingUI::LogToConsole(
        MeetingUI::LogCategory::Connection,
        "SESSION_INVALIDATED",
        QString("[Coordinator] Stop room for invalidated account session, reason=%1")
            .arg(static_cast<int>(reason)));

    if (_state == MeetingState::Idle) {
        return;
    }

    QPointer<MeetingCoordinator> owner(this);
    if (_state != MeetingState::Leaving) {
        setState(MeetingState::Leaving, QCoreApplication::translate("MeetingUI", "Account session expired. Stopping the meeting..."));
    }
    if (!owner) {
        return;
    }
    owner->stopRoomSession();
    if (!owner) {
        return;
    }
    owner->setState(MeetingState::Idle, QCoreApplication::translate("MeetingUI", "Account session expired"));
}

void MeetingCoordinator::startRoomSession(const QString &url,
                                          const QString &token,
                                          uint64_t admissionGeneration) {
    if (!isAdmissionCurrent(admissionGeneration, AdmissionStage::ReadyToStart)) {
        qInfo() << "[Coordinator] Refuse to start room after session invalidation.";
        return;
    }
    _admissionStage = AdmissionStage::Starting;
    QPointer<MeetingCoordinator> owner(this);
    stopRoomSession(); // 确保前序会话已释放
    if (!owner || !owner->isAdmissionCurrent(admissionGeneration, AdmissionStage::Starting)) {
        return;
    }

    owner->setState(MeetingState::ConnectingRoom, QCoreApplication::translate("MeetingUI", "Establishing a WebRTC connection..."));
    if (!owner || !owner->isAdmissionCurrent(admissionGeneration, AdmissionStage::Starting)) {
        return;
    }
    owner->_admissionStage = AdmissionStage::Consumed;
    owner->_startupCommitted = false;
    owner->_startupReconnectPending = false;
    owner->_startupListenOnly = false;
    auto roomStartHook = owner->_roomStartHook;
    if (roomStartHook) {
        roomStartHook(url, token);
        return;
    }

    _sessionRunning = true;
    _ioContext = std::make_unique<asio::io_context>();
    _workGuard = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(_ioContext->get_executor());
    _sessionRuntime = std::make_shared<MeetingSessionRuntime>(
        *_ioContext,
        ++_nextSessionGeneration,
        _sessionManager.userId());

    _room = livekit::Room::Create(_ioContext->get_executor());
    attachAdmissionTelemetry(_sessionRuntime->telemetry());
    _startupTelemetry = _sessionRuntime->telemetry();
    _startupTelemetryOperationId = _sessionRuntime->telemetry()->StartOperation(
        livekit::telemetry::OperationKind::Startup);
    _room->SetSessionTelemetry(_sessionRuntime->telemetry());
    {
        auto session = _sessionRuntime;
        auto room = _room;
        const auto generation = session->generation();
        asio::post(session->strand(), [this, session, room, generation] {
            auto telemetry = session->telemetry();
            telemetry->SetSnapshotCallbackOnStrand(
                [this, generation](livekit::telemetry::SessionTelemetry::SnapshotPtr snapshot) {
                    if (const auto store =
                            livekit::telemetry::InstalledTelemetryHistoryStore()) {
                        const auto ledger =
                            livekit::telemetry::InstalledStabilityLedger();
                        store->SubmitSnapshot(
                            snapshot, ledger ? ledger->Summary()
                                             : livekit::telemetry::StabilitySummary{});
                    }
                    QMetaObject::invokeMethod(this,
                        [this, generation, snapshot = std::move(snapshot)] {
                            if (!isCurrentSessionGenerationOnUiThread(generation)) return;
                            const auto projection = ProjectTelemetrySnapshot(*snapshot);
                            emit telemetrySnapshotChanged(projection);
                        },
                        Qt::QueuedConnection);
                });
            telemetry->StartStatsSamplingOnStrand(
                [weak_room = std::weak_ptr<livekit::Room>(room)](
                    livekit::telemetry::SessionTelemetry::LateCompletion late_completion)
                    -> asio::awaitable<livekit::RoomStatsReport> {
                    if (auto current = weak_room.lock()) {
                        co_return co_await current->GetStats(std::move(late_completion));
                    }
                    co_return livekit::RoomStatsReport{};
                });
            auto process_resource_sampler =
                std::make_shared<livekit::telemetry::ProcessResourceSampler>();
            telemetry->StartRuntimeSamplingOnStrand(
                [process_resource_sampler] {
                    return process_resource_sampler->Sample();
                },
                [this, weak_telemetry =
                           std::weak_ptr<livekit::telemetry::SessionTelemetry>(telemetry)](
                    std::uint64_t telemetry_generation,
                    std::uint64_t probe_id,
                    livekit::telemetry::SessionTelemetry::Clock::time_point) {
                    QMetaObject::invokeMethod(this,
                        [this, weak_telemetry, telemetry_generation, probe_id] {
                            if (!isCurrentSessionGenerationOnUiThread(
                                    telemetry_generation)) {
                                return;
                            }
                            if (const auto current = weak_telemetry.lock()) {
                                current->CompleteUiLagProbe(
                                    telemetry_generation, probe_id);
                            }
                        },
                        Qt::QueuedConnection);
                });
            session->screenShareOnStrand() = std::make_shared<livekit::ScreenShareSession>(
                session->strand(), livekit::ScreenShareSession::ForRoom(room),
                [this, generation](livekit::ScreenShareSnapshot snapshot) {
                    QMetaObject::invokeMethod(this, [this, generation, snapshot] {
                        applyScreenShareSnapshotOnUiThread(generation, snapshot);
                    }, Qt::QueuedConnection);
                });
        });
    }
    _room->SetLogHandler([](const std::string &cat, const std::string &tag, const std::string &msg) {
        MeetingUI::LogCategory c = MeetingUI::LogCategory::General;
        if (cat == "WEBRTC") c = MeetingUI::LogCategory::WebRTC;
        else if (cat == "SIGNAL") c = MeetingUI::LogCategory::Signal;
        else if (cat == "TRACK") c = MeetingUI::LogCategory::Track;
        else if (cat == "ERROR") c = MeetingUI::LogCategory::Error;
        else if (cat == "MEDIA") c = MeetingUI::LogCategory::Media;
        MeetingUI::LogToConsole(c, QString::fromStdString(tag), QString::fromStdString(msg));
    });

    _roomListener = std::make_shared<CoordinatorRoomListener>(this, _sessionRuntime);
    _room->AddListener(_roomListener);

    // 确保本地音频与视频源就绪（复用已有实例，避免重复创建断开外设采集绑定）
    if (!_localAudioSource) {
        _localAudioSource = std::make_shared<livekit::AudioSource>(48000, 2);
    }
    if (!_localVideoSource) {
        _localVideoSource = std::make_shared<livekit::VideoSource>(1280, 720);
    }

    const std::string urlStr = url.toStdString();
    const std::string tokenStr = token.toStdString();
    auto *ioContext = _ioContext.get();
    auto room = _room;
    auto session = _sessionRuntime;
    const uint64_t sessionGeneration = session->generation();
    auto audioSource = _localAudioSource;
    auto videoSource = _localVideoSource;
    const bool audioMuted = _requestedAudioMuted;
    const bool videoEnabled = _requestedVideoEnabled;
    const bool audioAvailable = _localAudioAvailable;
    const bool videoAvailable = _localVideoAvailable;
    const bool allowInsecureTransport = isDebugHttpTransportEnabled();

    _ioThread = std::thread([this, ioContext, room = std::move(room), session = std::move(session),
                             audioSource = std::move(audioSource), videoSource = std::move(videoSource),
                             audioMuted, videoEnabled, audioAvailable, videoAvailable, allowInsecureTransport,
                             urlStr, tokenStr, sessionGeneration] {
        livekit::SignalOptions opts;
        opts.auto_subscribe = true;
        opts.connect_timeout = std::chrono::seconds(10);
        opts.allow_insecure_transport = allowInsecureTransport;

        asio::co_spawn(*ioContext,
                        [this, room = std::move(room), session = std::move(session),
                         audioSource = std::move(audioSource), videoSource = std::move(videoSource),
                         audioMuted, videoEnabled, audioAvailable, videoAvailable,
                         urlStr, tokenStr, opts, sessionGeneration]() mutable -> asio::awaitable<void> {
            MeetingStartupTransaction startup;
            try {
                if (urlStr.empty() || tokenStr.empty()) {
                    throw std::runtime_error(QCoreApplication::translate("MeetingUI", "The LiveKit URL or access token is empty").toStdString());
                }

                co_await room->ConnectAsync(urlStr, tokenStr, opts);
                if (!startup.markRoomConnected()) {
                    throw std::runtime_error(QCoreApplication::translate("MeetingUI", "Invalid local media startup transaction state").toStdString());
                }
                MediaPreferences requestedPreferences;
                requestedPreferences.enableMicrophone = !audioMuted;
                requestedPreferences.enableVideo = videoEnabled;
                const auto initialMedia = resolveInitialMediaState(
                    room->room_info().metadata, requestedPreferences);
                const bool effectiveAudioMuted = !initialMedia.microphoneEnabled;
                const bool effectiveVideoEnabled = initialMedia.videoEnabled;
                MeetingUI::LogToConsole(
                    MeetingUI::LogCategory::Media,
                    "INITIAL_MEDIA_POLICY",
                    QString("metadata=%1 valid=%2 microphone=%3 camera=%4")
                        .arg(initialMedia.hasOpenMeetingDetail ? QStringLiteral("openmeeting")
                                                              : QStringLiteral("none"))
                        .arg(initialMedia.metadataValid ? QStringLiteral("true")
                                                        : QStringLiteral("false"))
                        .arg(initialMedia.microphoneEnabled ? QStringLiteral("enabled")
                                                            : QStringLiteral("disabled"))
                        .arg(initialMedia.videoEnabled ? QStringLiteral("enabled")
                                                       : QStringLiteral("disabled")));
                QMetaObject::invokeMethod(this, [this, sessionGeneration]() {
                    if (!isCurrentSessionGenerationOnUiThread(sessionGeneration)) {
                        return;
                    }
                    // 房间底层信令与下行通道已就绪，立即进入 InMeeting 状态以秒级呈现远端画面
                    setState(MeetingState::InMeeting,
                             QCoreApplication::translate("MeetingUI", "Connected to the meeting room. Starting local audio and video..."));
                    emit meetingJoinedSuccessfully(_currentMeetingId);
                }, Qt::QueuedConnection);

                auto local = room->local_participant();
                if (!local) {
                    throw std::runtime_error(QCoreApplication::translate("MeetingUI", "The LiveKit room connected without creating a local participant").toStdString());
                }

                auto audioTrack = livekit::LocalAudioTrack::createLocalAudioTrack("simple_audio", audioSource);
                audioTrack->set_muted(effectiveAudioMuted || !audioAvailable);

                livekit::VideoPublishOptions vopts;
                vopts.video_codec = "vp8";
                auto videoTrack = livekit::LocalVideoTrack::createLocalVideoTrack(
                    "camera_video", videoSource, livekit::TrackSource::Camera, vopts);
                videoTrack->set_muted(!effectiveVideoEnabled || !videoAvailable);

                // 【核心优化】：将本地音视频打包，发起批量发布与单次全量 SDP 协商
                std::vector<std::shared_ptr<livekit::Track>> tracksToPublish;
                tracksToPublish.push_back(audioTrack);
                tracksToPublish.push_back(videoTrack);

                auto pubs = co_await local->PublishTracksBatchAsync(std::move(tracksToPublish));
                if (!startup.markMediaBatchPublished()) {
                    throw std::runtime_error(QCoreApplication::translate("MeetingUI", "Invalid local media batch publication transaction state").toStdString());
                }
                MeetingUI::LogToConsole(MeetingUI::LogCategory::Track, "PUBLISH",
                    QCoreApplication::translate("MeetingUI", "Local audio/video published (%1 tracks, a single combined SDP negotiation)").arg(pubs.size()));

                QMetaObject::invokeMethod(this,
                                          [this, sessionGeneration, audioTrack = std::move(audioTrack),
                                           videoTrack = std::move(videoTrack), audioMuted, videoEnabled,
                                           effectiveAudioMuted, effectiveVideoEnabled]() mutable {
                    completeRoomStartupOnUiThread(
                        sessionGeneration, std::move(audioTrack), std::move(videoTrack),
                        audioMuted, videoEnabled, effectiveAudioMuted, effectiveVideoEnabled);
                }, Qt::QueuedConnection);
            } catch (const std::exception &) {
                const QString err = QString::fromStdString(
                    livekit::secure_log::ExceptionSummary("meeting_startup"));
                const bool mediaBegan = startup.mediaStartupBegan();
                const QString title = mediaBegan
                    ? QCoreApplication::translate("MeetingUI", "Local Media Startup Failed")
                    : QCoreApplication::translate("MeetingUI", "Room Connection Failed");
                if (startup.beginRollback()) {
                    startup.completeRollback();
                }
                MeetingUI::LogToConsole(MeetingUI::LogCategory::Error, "STARTUP_TRANSACTION",
                                        QString("%1: %2").arg(title, err));
                if (mediaBegan) {
                    // 房间本身连接正常，仅本地媒体硬件发布异常：降级为无媒体参会，不强制断开会议
                    QMetaObject::invokeMethod(this, [this, sessionGeneration, title, err]() {
                        completeRoomStartupDegradedOnUiThread(sessionGeneration, title, err);
                    }, Qt::QueuedConnection);
                } else {
                    // 连接房间本身失败：执行回滚并清理
                    QMetaObject::invokeMethod(this, [this, sessionGeneration, title, err]() {
                        failRoomStartupOnUiThread(sessionGeneration, title, err);
                    }, Qt::QueuedConnection);
                }
            }
        }, asio::detached);

        ioContext->run();
    });
}

void MeetingCoordinator::completeRoomStartupOnUiThread(
    uint64_t sessionGeneration,
    std::shared_ptr<livekit::LocalAudioTrack> audioTrack,
    std::shared_ptr<livekit::LocalVideoTrack> videoTrack,
    bool requestedAudioMuted,
    bool requestedVideoEnabled,
    bool effectiveAudioMuted,
    bool effectiveVideoEnabled) {
    if (!isCurrentSessionGenerationOnUiThread(sessionGeneration)) {
        return;
    }
    if (_startupCommitted) {
        return;
    }

    QPointer<MeetingCoordinator> owner(this);
    if (_requestedAudioMuted == requestedAudioMuted) {
        _requestedAudioMuted = effectiveAudioMuted;
    }
    if (_requestedVideoEnabled == requestedVideoEnabled) {
        _requestedVideoEnabled = effectiveVideoEnabled;
    }
    // Device readiness is temporary; the join policy and explicit user intent
    // survive a capture restart so recovery cannot undo a requested mute.
    _audioMuted = _requestedAudioMuted || !_localAudioAvailable;
    _videoEnabled = _requestedVideoEnabled && _localVideoAvailable;
    _localAudioTrack = std::move(audioTrack);
    _localVideoTrack = std::move(videoTrack);
    // AddTrack carries the initial state. A device failure or user action may
    // have changed it while publication was pending; send the final state too.
    publishLocalTrackMute(_localAudioTrack, _audioMuted);
    publishLocalTrackMute(_localVideoTrack, !_videoEnabled);
    _startupListenOnly = false;
    _startupCommitted = true;
    finishStartupTelemetry(livekit::telemetry::OperationOutcome::Success);
    finishAdmissionTelemetry(
        _admissionGeneration, livekit::telemetry::OperationOutcome::Success);
    ensureLocalParticipant();
    for (auto &[_, participant] : _participants) {
        if (!participant.isLocal) continue;
        participant.isAudioMuted = _audioMuted;
        participant.isVideoEnabled = _videoEnabled;
        break;
    }
    if (!tryCommitOperationalStateOnUiThread(
            sessionGeneration, QCoreApplication::translate("MeetingUI", "Local audio and video are ready. Connected to the meeting room."))) {
        return;
    }
    updateParticipantListAndNotify();
    if (!owner || !owner->isCurrentSessionGenerationOnUiThread(sessionGeneration)) {
        return;
    }
    emit owner->localAudioMuteChanged(owner->_audioMuted);
    if (!owner || !owner->isCurrentSessionGenerationOnUiThread(sessionGeneration)) {
        return;
    }
    emit owner->localVideoEnableChanged(owner->_videoEnabled);
}

void MeetingCoordinator::completeRoomStartupDegradedOnUiThread(
    uint64_t sessionGeneration,
    const QString &title,
    const QString &detail) {
    if (!isCurrentSessionGenerationOnUiThread(sessionGeneration) || _startupCommitted) {
        return;
    }

    QPointer<MeetingCoordinator> owner(this);
    _localAudioTrack.reset();
    _localVideoTrack.reset();
    _audioMuted = true;
    _videoEnabled = false;
    _startupListenOnly = true;
    ensureLocalParticipant();
    for (auto &[_, participant] : _participants) {
        if (!participant.isLocal) continue;
        participant.isAudioMuted = true;
        participant.isVideoEnabled = false;
        break;
    }
    _startupCommitted = true;
    finishStartupTelemetry(
        livekit::telemetry::OperationOutcome::DegradedSuccess);
    finishAdmissionTelemetry(
        _admissionGeneration,
        livekit::telemetry::OperationOutcome::DegradedSuccess);
    if (!tryCommitOperationalStateOnUiThread(
            sessionGeneration, QCoreApplication::translate("MeetingUI", "Local media is unavailable. Switched to receive-only mode."))) {
        return;
    }

    updateParticipantListAndNotify();
    if (!owner || !owner->isCurrentSessionGenerationOnUiThread(sessionGeneration)) {
        return;
    }
    emit owner->localAudioMuteChanged(true);
    if (!owner || !owner->isCurrentSessionGenerationOnUiThread(sessionGeneration)) {
        return;
    }
    emit owner->localVideoEnableChanged(false);
    if (!owner || !owner->isCurrentSessionGenerationOnUiThread(sessionGeneration)) {
        return;
    }
    emit owner->errorOccurred(
        title, QCoreApplication::translate("MeetingUI", "%1 (automatically switched to receive-only mode)").arg(detail));
}

bool MeetingCoordinator::tryCommitOperationalStateOnUiThread(
    uint64_t sessionGeneration,
    const QString &detail) {
    if (!isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
        _state == MeetingState::Leaving || _state == MeetingState::Idle) {
        return false;
    }
    if (!_startupCommitted || _startupReconnectPending) {
        return true;
    }

    QPointer<MeetingCoordinator> owner(this);
    setState(MeetingState::InMeeting, detail);
    return owner && owner->isCurrentSessionGenerationOnUiThread(sessionGeneration) &&
        owner->_state == MeetingState::InMeeting;
}

void MeetingCoordinator::failRoomStartupOnUiThread(
    uint64_t sessionGeneration,
    const QString &title,
    const QString &detail) {
    if (!isCurrentSessionGenerationOnUiThread(sessionGeneration)) {
        return;
    }

    finishStartupTelemetry(livekit::telemetry::OperationOutcome::Failure);
    finishAdmissionTelemetry(
        _admissionGeneration, livekit::telemetry::OperationOutcome::Failure);

    // The target protocol has no local media-Unpublish request.  A full Room
    // disconnect is therefore the only server-visible rollback that cannot
    // leave an audio-only or video-only startup publication behind.
    setState(MeetingState::Leaving, QCoreApplication::translate("MeetingUI", "Local media startup failed. Rolling back the room session..."));
    stopRoomSession();
    setState(MeetingState::Failed, detail);
    emit errorOccurred(title, detail);
}

void MeetingCoordinator::stopRoomSession() {
    finishStartupTelemetry(livekit::telemetry::OperationOutcome::Cancelled);
    ++_screenSourceRequest;
    _screenShareSnapshot = {};
    _startupCommitted = false;
    _startupReconnectPending = false;
    _startupListenOnly = false;
    if (_whiteboardTickTimer) _whiteboardTickTimer->stop();
    const bool was_running = _sessionRunning.exchange(false);
    if (!was_running) {
        finishActiveStabilitySession();
        return;
    }
    QPointer<MeetingCoordinator> owner(this);
    const auto stoppedGeneration = _nextSessionGeneration;
    std::vector<QString> failedOutboundMessages;
    QString transferCleanupError;

    if (_mediaSendTimer && _mediaSendTimer->isActive()) {
        _mediaSendTimer->stop();
    }
    for (const auto &task : _mediaSendQueue) {
        if (!task.messageId.isEmpty()) {
            failedOutboundMessages.push_back(task.messageId);
        }
    }
    _mediaSendQueue.clear();

    std::vector<QString> failedInboundTransfers;
    auto session = _sessionRuntime;
    if (session && _ioContext && _ioThread.joinable()) {
        // Serialize the admission barrier and transfer cleanup behind any
        // in-flight DataChannel callback before this thread stops the executor.
        auto cleanup = std::make_shared<std::promise<std::vector<QString>>>();
        auto completed = cleanup->get_future();
        asio::post(session->strand(), [session, cleanup]() {
            session->assertOnStrand();
            session->stopAcceptingDataOnStrand([session, cleanup] {
                try {
                    session->assertOnStrand();
                    if (auto &share = session->screenShareOnStrand()) {
                        share->Shutdown();
                        share.reset();
                    }
                    if (auto &board = session->whiteboardOnStrand()) {
                        board->retire();
                        board.reset();
                    }

                    std::vector<QString> failed;
                    auto &transfers = session->transfersOnStrand();
                    failed.reserve(transfers.size());
                    for (const auto &[transferId, _] : transfers) {
                        failed.push_back(transferId.uiTransferId());
                    }
                    transfers.clear();
                    cleanup->set_value(std::move(failed));
                } catch (...) {
                    cleanup->set_exception(std::current_exception());
                }
            });
        });
        try {
            failedInboundTransfers = completed.get();
        } catch (const std::exception &) {
            transferCleanupError = QString::fromStdString(
                livekit::secure_log::ExceptionSummary("transfer_cleanup"));
        }
    }
    for (const auto &[uiTransferId, entry] : _inboundTransferLedger) {
        if (entry.second) {
            failedInboundTransfers.erase(std::remove(failedInboundTransfers.begin(),
                failedInboundTransfers.end(), uiTransferId), failedInboundTransfers.end());
            continue;
        }
        if (std::find(failedInboundTransfers.begin(), failedInboundTransfers.end(), uiTransferId) ==
            failedInboundTransfers.end()) {
            failedInboundTransfers.push_back(uiTransferId);
        }
    }
    _inboundTransferLedger.clear();

    if (_wasapiCap) {
        _wasapiCap->Stop();
        _wasapiCap.reset();
    }
    if (_dshowCap) {
        _dshowCap->Stop();
        _dshowCap.reset();
    }

    if (_room) {
        // External owners may keep Room alive after this Coordinator. Revoke
        // its bridge registration before disconnect; the worker join below
        // still completes any listener batch already taken by the dispatcher.
        if (_roomListener) _room->RemoveListener(_roomListener);
        _room->Disconnect();
    }

    if (_workGuard) {
        _workGuard->reset();
    }
    if (_ioContext) {
        _ioContext->stop();
    }
    if (_ioThread.joinable()) {
        _ioThread.join();
    }

    // A MeetingSessionRuntime contains an asio::strand. Destroy every owner
    // of that runtime while its io_context and strand service still exist.
    // In particular, no queued Qt callback may retain the runtime; those
    // callbacks carry only the immutable session generation.
    _roomListener.reset();
    _room.reset();
    _sessionRuntime.reset();
    session.reset();
    _workGuard.reset();
    _ioContext.reset();
    _localAudioTrack.reset();
    _localVideoTrack.reset();
    _localAudioSource.reset();
    _localVideoSource.reset();
    _remoteVideoTracks.clear();
    _participantEventSequences.clear();
    _nativeRoomGeneration = 0;
    _whiteboardSnapshot.clear();
    _whiteboardSequence = 0;
    _whiteboardState = static_cast<int>(livekit::whiteboard::CollaborationState::Retired);
    _whiteboardAuthority.clear();
    _whiteboardLocalActor.clear();
    _whiteboardLocked = false;
    _whiteboardWritersOpen = true;
    _whiteboardCanEdit = false;
    _whiteboardCanAdmin = false;
    _whiteboardAssets.clear();
    _whiteboardStatus.clear();
    finishActiveStabilitySession();

    // Finish resource cleanup before notifications can synchronously delete
    // the owner or start another session. Never clean up that successor.
    const auto stillStopped = [&] {
        return owner && owner->_nextSessionGeneration == stoppedGeneration &&
            !owner->_sessionRunning.load(std::memory_order_acquire);
    };
    if (!transferCleanupError.isEmpty()) {
        MeetingUI::LogToConsole(MeetingUI::LogCategory::Error,
            "SESSION_TRANSFER_CLEANUP", transferCleanupError);
        if (!stillStopped()) return;
    }
    for (const auto &messageId : failedOutboundMessages) {
        emit owner->chatMessageSendFailed(messageId, QCoreApplication::translate("MeetingUI", "Meeting Left"));
        if (!stillStopped()) return;
    }
    for (const auto &transferId : failedInboundTransfers) {
        emit owner->chatMediaReceivingFailed(transferId, QCoreApplication::translate("MeetingUI", "You have left the meeting"));
        if (!stillStopped()) return;
    }
}

void MeetingCoordinator::applyScreenShareSnapshotOnUiThread(uint64_t generation, livekit::ScreenShareSnapshot snapshot) {
    if (!isCurrentSessionGenerationOnUiThread(generation) ||
        (_state != MeetingState::InMeeting && _state != MeetingState::Reconnecting)) return;
    _screenShareSnapshot = snapshot;
    emit screenShareChanged(snapshot);
}

void MeetingCoordinator::requestScreenShareSources() {
    if (!_sessionRuntime || !_sessionRunning || _state != MeetingState::InMeeting || !_startupCommitted) {
        emit errorOccurred(QCoreApplication::translate("MeetingUI", "Screen Sharing"), QCoreApplication::translate("MeetingUI", "Wait for the connection and local media setup to complete"));
        return;
    }
    const auto request = ++_screenSourceRequest;
    auto session = _sessionRuntime;
    const auto generation = session->generation();
    asio::post(session->strand(), [this, session, generation, request] {
        if (!session->acceptsDataOnStrand()) return;
        std::vector<livekit::DesktopSource> sources;
        try { sources = livekit::EnumerateDesktopSources(); } catch (...) {}
        QMetaObject::invokeMethod(this, [this, generation, request, sources = std::move(sources)] {
            if (!isCurrentSessionGenerationOnUiThread(generation) || request != _screenSourceRequest ||
                _state != MeetingState::InMeeting) return;
            emit screenShareSourcesReady(sources);
        }, Qt::QueuedConnection);
    });
}

void MeetingCoordinator::startScreenShare(livekit::DesktopSource source) {
    if (!_sessionRuntime || !_sessionRunning || _state != MeetingState::InMeeting || !_startupCommitted) return;
    auto session = _sessionRuntime;
    asio::post(session->strand(), [session, source = std::move(source)] {
        if (!session->acceptsDataOnStrand()) return;
        if (auto share = session->screenShareOnStrand()) share->Start(source);
    });
}

void MeetingCoordinator::stopScreenShare() {
    ++_screenSourceRequest;
    if (!_sessionRuntime || !_sessionRunning) return;
    auto session = _sessionRuntime;
    asio::post(session->strand(), [session] {
        if (auto share = session->screenShareOnStrand()) share->Stop();
    });
}

void MeetingCoordinator::publishLocalTrackMute(
    const std::shared_ptr<livekit::Track> &track, bool muted) {
    if (!track || !_sessionRuntime || !_sessionRunning || !_room) return;
    auto session = _sessionRuntime;
    auto room = _room;
    auto local = room->local_participant();
    if (!local) return;
    asio::post(session->strand(), [session, room, local, track, muted] {
        if (!session->acceptsDataOnStrand() || room->local_participant() != local) return;
        // Resolve the SID from the live publication, never from a track CID or
        // a retired session. SetMuted applies native state and informs the SFU.
        for (const auto &[sid, publication] : local->tracks()) {
            if (!sid.empty() && publication && publication->track() == track) {
                local->SetMuted(sid, muted);
                return;
            }
        }
    });
}

void MeetingCoordinator::setLocalAudioAvailable(bool available) {
    if (_localAudioAvailable == available) return;
    _localAudioAvailable = available;
    applyLocalAudioState();
}

void MeetingCoordinator::setLocalVideoAvailable(bool available) {
    if (_localVideoAvailable == available) return;
    _localVideoAvailable = available;
    applyLocalVideoState();
}

void MeetingCoordinator::setLocalAudioMuted(bool muted) {
    _requestedAudioMuted = muted;
    applyLocalAudioState();
}

void MeetingCoordinator::applyLocalAudioState() {
    const bool muted = _requestedAudioMuted || _startupListenOnly || !_localAudioAvailable;
    QPointer<MeetingCoordinator> owner(this);
    const auto generation = _nextSessionGeneration;
    _audioMuted = muted;
    publishLocalTrackMute(_localAudioTrack, muted);
    for (auto &[id, p] : _participants) {
        if (p.isLocal) {
            p.isAudioMuted = muted;
            break;
        }
    }
    updateParticipantListAndNotify();
    if (owner && owner->_nextSessionGeneration == generation && owner->_audioMuted == muted) {
        emit owner->localAudioMuteChanged(muted);
    }
}

void MeetingCoordinator::setLocalVideoEnabled(bool enabled) {
    _requestedVideoEnabled = enabled;
    applyLocalVideoState();
}

void MeetingCoordinator::applyLocalVideoState() {
    const bool enabled = _requestedVideoEnabled && !_startupListenOnly && _localVideoAvailable;
    QPointer<MeetingCoordinator> owner(this);
    const auto generation = _nextSessionGeneration;
    _videoEnabled = enabled;
    publishLocalTrackMute(_localVideoTrack, !enabled);
    for (auto &[id, p] : _participants) {
        if (p.isLocal) {
            p.isVideoEnabled = enabled;
            break;
        }
    }
    updateParticipantListAndNotify();
    if (owner && owner->_nextSessionGeneration == generation && owner->_videoEnabled == enabled) {
        emit owner->localVideoEnableChanged(enabled);
    }
}

void MeetingCoordinator::sendNotifyData(const openmeeting::meeting::NotifyMeetingData &data, bool reliable) {
    if (!_room || _state != MeetingState::InMeeting) return;
    std::string bytes;
    if (!data.SerializeToString(&bytes)) return;
    std::vector<uint8_t> payload(bytes.begin(), bytes.end());
    _room->PublishData(payload, reliable);
}

int64_t MeetingCoordinator::nextSequenceNumber() {
    int64_t now = QDateTime::currentMSecsSinceEpoch() * 1000;
    int64_t counter = ++_msgSequenceCounter;
    return now + (counter % 1000);
}

void MeetingCoordinator::sendChatMessage(const QString &content, const QString &messageId, int64_t seq) {
    if (content.isEmpty()) return;
    if (!_room || _state != MeetingState::InMeeting) {
        if (!messageId.isEmpty()) {
            emit chatMessageSendFailed(messageId, QCoreApplication::translate("MeetingUI", "Not connected to a meeting room"));
        }
        return;
    }
    if (seq <= 0) {
        seq = nextSequenceNumber();
    }

    QJsonObject obj;
    obj["om_type"] = "chat_text";
    obj["seq"] = static_cast<double>(seq);
    obj["msgId"] = messageId;
    obj["text"] = content;

    QJsonDocument doc(obj);
    QByteArray jsonBytes = doc.toJson(QJsonDocument::Compact);
    std::vector<uint8_t> payload(jsonBytes.begin(), jsonBytes.end());
    try {
        bool ok = _room->PublishData(payload, true, {}, "chat");
        if (ok) {
            if (!messageId.isEmpty()) {
                QMetaObject::invokeMethod(this, [this, messageId]() {
                    emit chatMessageSendProgress(messageId, 100);
                    emit chatMessageSendSuccess(messageId);
                }, Qt::QueuedConnection);
            }
        } else {
            if (!messageId.isEmpty()) {
                emit chatMessageSendFailed(messageId, QCoreApplication::translate("MeetingUI", "Data channel congested. Send failed."));
            }
        }
    } catch (const std::exception &) {
        if (!messageId.isEmpty()) {
            emit chatMessageSendFailed(
                messageId,
                QString::fromStdString(
                    livekit::secure_log::ExceptionSummary("chat_send")));
        }
    } catch (...) {
        if (!messageId.isEmpty()) {
            emit chatMessageSendFailed(messageId, QCoreApplication::translate("MeetingUI", "An unknown error occurred while sending"));
        }
    }
}

void MeetingCoordinator::sendChatMediaMessage(const QString &messageId, const QString &mediaType, const QString &fileName, const QByteArray &data, int64_t seq) {
    if (data.isEmpty()) {
        if (!messageId.isEmpty()) {
            emit chatMessageSendFailed(messageId, QCoreApplication::translate("MeetingUI", "No data to send"));
        }
        return;
    }
    if (!_room || _state != MeetingState::InMeeting) {
        if (!messageId.isEmpty()) {
            emit chatMessageSendFailed(messageId, QCoreApplication::translate("MeetingUI", "Not connected to a meeting room"));
        }
        return;
    }
    if (seq <= 0) {
        seq = nextSequenceNumber();
    }

    QString base64 = QString::fromLatin1(data.toBase64());
    const int chunkSize = 10 * 1024; // 每分片 10KB 字符，确保加上 JSON 协议头后小于 15KB，避免触发 DataStream 二次切片与竞争
    const int totalLen = base64.length();
    const int totalChunks = (totalLen + chunkSize - 1) / chunkSize;
    const QString transferId = QString("media_%1_%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(qrand() % 10000);

    // 1. 发送轻量预告包 media_start 瞬时建立接收端占位 (不等待切片，秒送达)
    QJsonObject startObj;
    startObj["om_type"] = "media_start";
    startObj["transferId"] = transferId;
    startObj["seq"] = static_cast<double>(seq);
    startObj["mediaType"] = mediaType;
    startObj["fileName"] = fileName;
    startObj["totalSize"] = static_cast<qint64>(data.size());
    startObj["totalChunks"] = totalChunks;

    QJsonDocument startDoc(startObj);
    QByteArray startBytes = startDoc.toJson(QJsonDocument::Compact);
    std::vector<uint8_t> startPayload(startBytes.begin(), startBytes.end());
    try {
        _room->PublishData(startPayload, true, {}, "chat");
    } catch (...) {
        // 忽略预告包偶发抛错，后续第一个 chunk 也会兜底建立占位
    }

    // 2. 将数据切片入队由 20ms 平滑流控调度
    MediaSendChunkTask task;
    task.messageId = messageId;
    task.mediaType = mediaType;
    task.fileName = fileName;
    task.transferId = transferId;
    task.totalSize = static_cast<qint64>(data.size());
    task.seq = seq;
    task.currentChunk = 0;
    task.totalChunks = totalChunks;
    task.base64Payload = base64;
    task.chunkSize = chunkSize;

    _mediaSendQueue.push_back(std::move(task));

    if (_mediaSendTimer && !_mediaSendTimer->isActive()) {
        _mediaSendTimer->start(20); // 每 20ms 推送一个分片
    }
}

void MeetingCoordinator::processNextMediaSendChunk() {
    if (_mediaSendQueue.empty()) {
        if (_mediaSendTimer && _mediaSendTimer->isActive()) {
            _mediaSendTimer->stop();
        }
        return;
    }

    if (!_room || _state != MeetingState::InMeeting) {
        while (!_mediaSendQueue.empty()) {
            auto task = _mediaSendQueue.front();
            _mediaSendQueue.pop_front();
            if (!task.messageId.isEmpty()) {
                emit chatMessageSendFailed(task.messageId, QCoreApplication::translate("MeetingUI", "Network disconnected"));
            }
        }
        if (_mediaSendTimer && _mediaSendTimer->isActive()) {
            _mediaSendTimer->stop();
        }
        return;
    }

    // === DataChannel 背压流控 (Backpressure Flow Control) ===
    // 检查底层 WebRTC SCTP 待发送缓冲区水位，门限设为 64KB
    // 若当前积压大于 64KB，暂停本轮推送，让底层网络充分排空，彻底避免打爆 SCTP 缓冲区与丢包
    uint64_t buffered = _room->GetDataChannelBufferedAmount(true);
    if (buffered > 64 * 1024) {
        return; // 等待下一个 20ms tick 再次检测
    }

    auto &task = _mediaSendQueue.front();
    int i = task.currentChunk;
    QString chunkStr = task.base64Payload.mid(i * task.chunkSize, task.chunkSize);
    QJsonObject obj;
    obj["om_type"] = "media_chunk";
    obj["transferId"] = task.transferId;
    obj["seq"] = static_cast<double>(task.seq);
    obj["chunkIndex"] = i;
    obj["totalChunks"] = task.totalChunks;
    obj["mediaType"] = task.mediaType;
    obj["fileName"] = task.fileName;
    obj["totalSize"] = task.totalSize;
    obj["chunkData"] = chunkStr;

    QJsonDocument doc(obj);
    QByteArray jsonBytes = doc.toJson(QJsonDocument::Compact);
    std::vector<uint8_t> payload(jsonBytes.begin(), jsonBytes.end());
    bool ok = false;
    try {
        ok = _room->PublishData(payload, true, {}, "chat");
    } catch (const std::exception &) {
        QString msgId = task.messageId;
        _mediaSendQueue.pop_front();
        if (!msgId.isEmpty()) {
            emit chatMessageSendFailed(
                msgId,
                QString::fromStdString(
                    livekit::secure_log::ExceptionSummary("data_packet_send")));
        }
        return;
    } catch (...) {
        QString msgId = task.messageId;
        _mediaSendQueue.pop_front();
        if (!msgId.isEmpty()) {
            emit chatMessageSendFailed(msgId, QCoreApplication::translate("MeetingUI", "An unknown error occurred while delivering the packet"));
        }
        return;
    }

    if (!ok) {
        // 底层 Send 拒绝 (网络缓冲区已满)，不推进 currentChunk，保留给下一轮 tick 重试
        return;
    }

    task.currentChunk++;
    int progress = std::min(100, (task.currentChunk * 100) / task.totalChunks);
    if (!task.messageId.isEmpty()) {
        emit chatMessageSendProgress(task.messageId, progress);
    }

    if (task.currentChunk >= task.totalChunks) {
        QString msgId = task.messageId;
        _mediaSendQueue.pop_front();
        if (!msgId.isEmpty()) {
            emit chatMessageSendSuccess(msgId);
        }
    }
}

void MeetingCoordinator::requestParticipantMute(const QString &targetUserId, bool isVideo, bool mute) {
    openmeeting::meeting::NotifyMeetingData notify;
    notify.set_operatoruserid(_sessionManager.userId().toStdString());

    auto *streamOp = notify.mutable_streamoperatedata();
    auto *op = streamOp->add_operation();
    op->set_userid(targetUserId.toStdString());
    if (isVideo) {
        op->set_cameraonentry(!mute);
    } else {
        op->set_microphoneonentry(!mute);
    }

    sendNotifyData(notify, true);
}

void MeetingCoordinator::requestParticipantCamera(const QString &targetUserId, bool enable) {
    requestParticipantMute(targetUserId, true, !enable);
}

void MeetingCoordinator::requestParticipantMicrophone(const QString &targetUserId, bool enable) {
    requestParticipantMute(targetUserId, false, !enable);
}

void MeetingCoordinator::muteAllParticipants(bool muteMic, bool /*allowSelfUnmute*/) {
    if (!_room || _state != MeetingState::InMeeting) return;
    openmeeting::meeting::NotifyMeetingData notify;
    notify.set_operatoruserid(_sessionManager.userId().toStdString());

    auto *streamOp = notify.mutable_streamoperatedata();
    for (const auto &[uid, info] : _participants) {
        if (!info.isLocal) {
            auto *op = streamOp->add_operation();
            op->set_userid(uid.toStdString());
            op->set_microphoneonentry(!muteMic);
        }
    }

    sendNotifyData(notify, true);
}

void MeetingCoordinator::transferHost(const QString &newHostUserId) {
    if (!_room || _state != MeetingState::InMeeting) return;
    openmeeting::meeting::NotifyMeetingData notify;
    notify.set_operatoruserid(SessionManager::instance().userId().toStdString());

    auto *hostData = notify.mutable_meetinghostdata();
    hostData->set_userid(newHostUserId.toStdString());
    hostData->set_operatornickname(SessionManager::instance().nickname().toStdString());
    hostData->set_hosttype("host");

    sendNotifyData(notify, true);

    _meetingDetail.hostUserId = newHostUserId;
    for (auto &[id, p] : _participants) {
        p.isHost = (id == newHostUserId);
    }
    updateParticipantListAndNotify();
    emit hostRoleChanged(newHostUserId, SessionManager::instance().nickname());
    emit meetingDetailUpdated(_meetingDetail);
}

void MeetingCoordinator::kickParticipant(const QString &targetUserId, const QString &reason) {
    openmeeting::meeting::NotifyMeetingData notify;
    notify.set_operatoruserid(SessionManager::instance().userId().toStdString());

    auto *kick = notify.mutable_kickoffmeetingdata();
    kick->set_userid(targetUserId.toStdString());
    kick->set_reason(reason.toStdString());
    kick->set_reasoncode(openmeeting::meeting::KickOffReason::Logout);

    sendNotifyData(notify, true);
}

void MeetingCoordinator::ensureLocalParticipant() {
    QString myId = _sessionManager.userId();
    if (myId.isEmpty() && _room && _room->local_participant()) {
        myId = QString::fromStdString(_room->local_participant()->identity());
    }
    if (myId.isEmpty()) {
        myId = _currentDisplayName.isEmpty() ? QString("local_user") : _currentDisplayName;
    }

    ParticipantInfo localInfo;
    localInfo.identity = myId;
    QString resolvedNick;
    if (_room && _room->local_participant()) {
        resolvedNick = ResolveParticipantNickname(_room->local_participant());
    }
    if (!resolvedNick.isEmpty() && resolvedNick != myId) {
        localInfo.name = resolvedNick;
    } else if (!_currentDisplayName.isEmpty()) {
        localInfo.name = _currentDisplayName;
    } else if (!_sessionManager.nickname().isEmpty()) {
        localInfo.name = _sessionManager.nickname();
    } else {
        localInfo.name = myId;
    }
    localInfo.isLocal = true;
    localInfo.isHost = isHost();
    localInfo.isAudioMuted = _audioMuted;
    localInfo.isVideoEnabled = _videoEnabled;
    if (_room && _room->local_participant()) {
        CopyParticipantState(_room->local_participant(), &localInfo);
    }

    // 清除旧的可能存在的本地项，避免重复
    for (auto it = _participants.begin(); it != _participants.end();) {
        if (it->second.isLocal) {
            it = _participants.erase(it);
        } else {
            ++it;
        }
    }
    _participants[myId] = localInfo;
}

std::vector<ParticipantInfo> MeetingCoordinator::participants() const {
    std::vector<ParticipantInfo> list;
    list.reserve(_participants.size());
    for (const auto &[_, info] : _participants) {
        list.push_back(info);
    }
    return list;
}

std::vector<ParticipantPresentation> MeetingCoordinator::participantPresentations() const {
    Q_ASSERT(QThread::currentThread() == thread());
    std::vector<ParticipantPresentation> result;
    result.reserve(_participants.size());
    for (const auto &[identity, info] : _participants) {
        ParticipantPresentation presentation;
        presentation.participant = info;
        presentation.coordinatorSession = _nextSessionGeneration;
        if (!isParticipantPresentationCurrent(presentation)) continue;
        const auto tracks = _remoteVideoTracks.find(identity);
        if (tracks != _remoteVideoTracks.end()) {
            for (const auto &[trackSid, value] : tracks->second) {
                if (isParticipantPresentationCurrent(presentation, &value)) {
                    presentation.videoTracks.push_back(value);
                }
            }
        }
        result.push_back(std::move(presentation));
    }
    return result;
}

bool MeetingCoordinator::isParticipantPresentationCurrent(
    const ParticipantPresentation &presentation,
    const RemoteVideoTrackPresentation *track) const {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto &info = presentation.participant;
    if (!isCurrentSessionGenerationOnUiThread(presentation.coordinatorSession) ||
        _state == MeetingState::Idle || _state == MeetingState::Leaving ||
        _state == MeetingState::Failed ||
        info.participantKey.native_room_generation != _nativeRoomGeneration ||
        !livekit::IsParticipantTicketActive(info.participantTicket, info.participantKey)) {
        return false;
    }
    const auto current = _participants.find(info.identity);
    if (current == _participants.end() || current->second.participantKey != info.participantKey ||
        current->second.lastEventSequence != info.lastEventSequence) return false;
    if (!track) return true;
    if (!track->track || track->key.participant != info.participantKey ||
        !livekit::IsTrackTicketActive(track->ticket, track->key) ||
        !livekit::IsMediaBindingTicketActive(
            track->mediaBindingTicket, track->mediaBindingKey)) return false;
    const auto tracks = _remoteVideoTracks.find(info.identity);
    if (tracks == _remoteVideoTracks.end()) return false;
    const auto currentTrack = tracks->second.find(QString::fromStdString(track->key.publication_sid));
    return currentTrack != tracks->second.end() && currentTrack->second.key == track->key &&
        currentTrack->second.mediaBindingKey == track->mediaBindingKey &&
        currentTrack->second.track == track->track;
}

void MeetingCoordinator::updateParticipantListAndNotify() {
    auto list = participants();
    emit participantsUpdated(list);
}

void MeetingCoordinator::parseRoomMetadata(const std::string &metadata) {
    QPointer<MeetingCoordinator> owner(this);
    const auto sessionGeneration = _nextSessionGeneration;
    const auto nativeGeneration = _nativeRoomGeneration;
    const auto current = [&] {
        return owner && owner->isCurrentSessionGenerationOnUiThread(sessionGeneration) &&
            owner->_nativeRoomGeneration == nativeGeneration;
    };
    if (metadata.empty()) {
        _meetingDetail = MeetingDetail{};
        for (auto &[identity, participant] : _participants) {
            participant.isHost = false;
        }
        updateParticipantListAndNotify();
        if (!current()) return;
        const auto detail = owner->_meetingDetail;
        emit owner->meetingDetailUpdated(detail);
        return;
    }
    const auto parsed = ParseOpenMeetingMetadata(metadata);
    if (parsed.state != OpenMeetingMetadataState::Valid) return;
    _meetingDetail = parsed.detail;

    for (auto &[identity, participant] : _participants) {
        participant.isHost = (identity == _meetingDetail.hostUserId ||
                              identity == _meetingDetail.creatorUserId);
    }

    updateParticipantListAndNotify();
    if (!current()) return;
    const auto updatedDetail = owner->_meetingDetail;
    emit owner->meetingDetailUpdated(updatedDetail);
    if (owner && owner->isCurrentSessionGenerationOnUiThread(sessionGeneration)) {
        owner->configureWhiteboardRuntimeOnUiThread();
    }
}

void MeetingCoordinator::configureWhiteboardRuntimeOnUiThread() {
    Q_ASSERT(QThread::currentThread() == thread());
    auto session = _sessionRuntime;
    auto room = _room;
    if (!session || !room || !_sessionRunning.load(std::memory_order_acquire)) return;
    const QString authority = !_meetingDetail.hostUserId.isEmpty()
        ? _meetingDetail.hostUserId : _meetingDetail.creatorUserId;
    QString localIdentity = session->localUserId();
    if (localIdentity.isEmpty() && room->local_participant())
        localIdentity = QString::fromStdString(room->local_participant()->identity());
    if (authority.isEmpty() || localIdentity.isEmpty()) return;
    const QByteArray boardKey = (_currentMeetingId + QLatin1Char('|') + _roomInfo.sid).toUtf8();
    const QString documentId = QStringLiteral("board-") + QString::fromLatin1(
        QCryptographicHash::hash(boardKey, QCryptographicHash::Sha256).toHex().left(32));
    const auto generation = session->generation();
    std::vector<livekit::whiteboard::PeerInstance> peers;
    peers.reserve(_participants.size());
    for (const auto &[identity, participant] : _participants) {
        if (participant.participantKey.incarnation == 0) continue;
        peers.push_back({identity.toStdString(),
            participant.participantKey.native_room_generation,
            participant.participantKey.incarnation});
    }
    auto transport = std::make_shared<livekit::whiteboard::RoomTransport>(room);
    livekit::whiteboard::RuntimeConfig config{
        documentId.toStdString(), localIdentity.toStdString(), authority.toStdString(),
        localIdentity == authority};
    const auto now = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    asio::post(session->strand(), [this, session, generation, config = std::move(config),
                                   peers = std::move(peers), transport, now]() mutable {
        if (!session->acceptsDataOnStrand() || session->whiteboardOnStrand()) return;
        auto projectedAssetIds = std::make_shared<std::set<std::string>>();
        auto project = [this, generation, projectedAssetIds](livekit::whiteboard::Projection value) mutable {
            QByteArray snapshot(value.snapshot.data(), static_cast<int>(value.snapshot.size()));
            const auto sequence = static_cast<quint64>(value.sequence);
            const auto state = static_cast<int>(value.state);
            QString authorityIdentity = QString::fromStdString(value.authorityIdentity);
            QString localActor = QString::fromStdString(value.localIdentity);
            QString status = QString::fromStdString(value.status);
            QVariantMap assets;
            for (const auto &asset : value.assets) {
                if (!asset || !projectedAssetIds->insert(asset->id).second) continue;
                assets.insert(QString::fromStdString(asset->id),
                    QByteArray(asset->bytes.data(), static_cast<int>(asset->bytes.size())));
            }
            QMetaObject::invokeMethod(this,
                [this, generation, snapshot = std::move(snapshot), sequence, state,
                 authorityIdentity = std::move(authorityIdentity), localActor = std::move(localActor),
                 locked = value.locked, writersOpen = value.writersOpen,
                 canEdit = value.canEdit, canAdmin = value.canAdmin,
                 assets = std::move(assets),
                 status = std::move(status)]() mutable {
                    projectWhiteboardOnUiThread(generation, std::move(snapshot), sequence, state,
                        std::move(authorityIdentity), std::move(localActor), locked, writersOpen,
                        canEdit, canAdmin, std::move(assets), std::move(status));
                }, Qt::QueuedConnection);
        };
        auto send = [transport](std::string_view topic, std::string_view payload,
                                const std::vector<std::string> &destinations) {
            return transport->send(topic, payload, destinations);
        };
        const auto authorityIdentity = config.authorityIdentity;
        auto runtime = std::make_shared<livekit::whiteboard::Runtime>(
            std::move(config), std::move(send), std::move(project));
        for (const auto &peer : peers) runtime->observePeer(peer);
        for (const auto &[identity, key] : session->whiteboardPeersOnStrand()) {
            runtime->observePeer({identity, key.native_room_generation, key.incarnation});
        }
        session->whiteboardOnStrand() = runtime;
        runtime->start(now);
        for (const auto &[identity, key] : session->whiteboardDeparturesOnStrand()) {
            if (identity == authorityIdentity)
                runtime->peerLeft({identity, key.native_room_generation, key.incarnation});
        }
    });
    if (!_whiteboardTickTimer->isActive()) _whiteboardTickTimer->start();
}

void MeetingCoordinator::activateWhiteboard() {
    Q_ASSERT(QThread::currentThread() == thread());
    configureWhiteboardRuntimeOnUiThread();
    if (_whiteboardSnapshot.isEmpty()) return;
    emit whiteboardProjectionChanged(_whiteboardSnapshot, _whiteboardSequence, _whiteboardState,
        _whiteboardAuthority, _whiteboardLocalActor, _whiteboardLocked, _whiteboardWritersOpen,
        _whiteboardCanEdit, _whiteboardCanAdmin, _whiteboardAssets, _whiteboardStatus);
}

void MeetingCoordinator::submitWhiteboardCommand(const livekit::whiteboard::Command &command) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto session = _sessionRuntime;
    if (!session || !_sessionRunning.load(std::memory_order_acquire)) return;
    const auto now = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    asio::post(session->strand(), [session, command, now] {
        if (!session->acceptsDataOnStrand()) return;
        if (auto board = session->whiteboardOnStrand()) board->propose(command, now);
    });
}

void MeetingCoordinator::submitWhiteboardImage(
    const QByteArray &png, const QString &assetId, int width, int height,
    const QString &pageId, bool replaceCurrent) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto session = _sessionRuntime;
    if (!session || !_sessionRunning.load(std::memory_order_acquire) || png.isEmpty()) return;
    livekit::whiteboard::Asset asset;
    asset.id = assetId.toStdString();
    asset.mime = "image/png";
    asset.width = static_cast<std::uint32_t>(std::max(0, width));
    asset.height = static_cast<std::uint32_t>(std::max(0, height));
    asset.bytes.assign(png.constData(), static_cast<std::size_t>(png.size()));
    const auto commandId = "image-op-" + asset.id.substr(0, 16) + "-" + pageId.toStdString();
    const auto now = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    asio::post(session->strand(),
        [session, asset = std::move(asset), commandId, pageId = pageId.toStdString(),
         replaceCurrent, now]() mutable {
            if (!session->acceptsDataOnStrand()) return;
            if (auto board = session->whiteboardOnStrand())
                board->importImage(std::move(asset), commandId, pageId, replaceCurrent, now);
        });
}

void MeetingCoordinator::setWhiteboardLocked(bool locked) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto session = _sessionRuntime;
    if (!session || !_sessionRunning.load(std::memory_order_acquire)) return;
    const auto now = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    asio::post(session->strand(), [session, locked, now] {
        if (auto board = session->whiteboardOnStrand()) board->setLocked(locked, now);
    });
}

void MeetingCoordinator::setWhiteboardWritersOpen(bool open) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto session = _sessionRuntime;
    if (!session || !_sessionRunning.load(std::memory_order_acquire)) return;
    const auto now = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    asio::post(session->strand(), [session, open, now] {
        if (auto board = session->whiteboardOnStrand()) board->setWriters(open, {}, now);
    });
}

void MeetingCoordinator::enqueueWhiteboardData(
    const std::shared_ptr<MeetingSessionRuntime> &session,
    const std::vector<uint8_t> &data,
    const std::string &topic,
    const livekit::SenderContext &sender) {
    if (!session || !_sessionRunning.load(std::memory_order_acquire) ||
        !livekit::whiteboard::isWhiteboardTopic(topic)) return;
    const auto now = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    asio::post(session->strand(), [session, data, topic, sender, now] {
        if (!session->acceptsDataOnStrand() ||
            !livekit::IsParticipantTicketActive(sender.ticket, sender.key)) return;
        livekit::whiteboard::PeerInstance peer{
            sender.key.identity.empty() ? sender.key.sid : sender.key.identity,
            sender.key.native_room_generation,
            sender.key.incarnation};
        session->whiteboardPeersOnStrand()[peer.identity] = sender.key;
        session->whiteboardDeparturesOnStrand().erase(peer.identity);
        auto board = session->whiteboardOnStrand();
        if (!board) return;
        board->observePeer(peer);
        board->receive(topic,
            std::string_view(reinterpret_cast<const char *>(data.data()), data.size()), peer, now);
    });
}

void MeetingCoordinator::projectWhiteboardOnUiThread(
    uint64_t sessionGeneration, QByteArray snapshot, quint64 sequence,
    int collaborationState, QString authorityIdentity, QString localActor,
    bool locked, bool writersOpen, bool canEdit, bool canAdmin,
    QVariantMap assets, QString status) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!isCurrentSessionGenerationOnUiThread(sessionGeneration)) return;
    _whiteboardSnapshot = std::move(snapshot);
    _whiteboardSequence = sequence;
    _whiteboardState = collaborationState;
    _whiteboardAuthority = std::move(authorityIdentity);
    _whiteboardLocalActor = std::move(localActor);
    _whiteboardLocked = locked;
    _whiteboardWritersOpen = writersOpen;
    _whiteboardCanEdit = canEdit;
    _whiteboardCanAdmin = canAdmin;
    for (auto it = assets.begin(); it != assets.end(); ++it)
        _whiteboardAssets.insert(it.key(), it.value());
    _whiteboardStatus = std::move(status);
    emit whiteboardProjectionChanged(_whiteboardSnapshot, _whiteboardSequence, _whiteboardState,
        _whiteboardAuthority, _whiteboardLocalActor, _whiteboardLocked, _whiteboardWritersOpen,
        _whiteboardCanEdit, _whiteboardCanAdmin, _whiteboardAssets, _whiteboardStatus);
}

void MeetingCoordinator::enqueueDataReceived(const std::shared_ptr<MeetingSessionRuntime> &session,
                                             const std::vector<uint8_t> &data,
                                             const livekit::SenderContext &sender) {
    if (!session || !_sessionRunning.load(std::memory_order_acquire)) {
        return;
    }

    asio::post(session->strand(), [this, session, data, sender]() {
        handleDataReceivedOnSessionStrand(session, data, sender);
    });
}

bool MeetingCoordinator::isCurrentSessionGenerationOnUiThread(uint64_t sessionGeneration) const {
    return sessionGeneration != 0 &&
        _sessionRunning.load(std::memory_order_acquire) &&
        _sessionRuntime &&
        _nextSessionGeneration == sessionGeneration &&
        _sessionRuntime->generation() == sessionGeneration;
}

bool MeetingCoordinator::isSenderContextCurrentOnUiThread(
    const livekit::SenderContext &sender) const {
    if (sender.origin == livekit::SenderOrigin::Server) return true;
    if (sender.origin == livekit::SenderOrigin::Unresolved ||
        !livekit::IsParticipantTicketActive(sender.ticket, sender.key)) {
        return false;
    }
    if (sender.origin == livekit::SenderOrigin::Local) return true;
    const QString identity = QString::fromStdString(
        sender.key.identity.empty() ? sender.key.sid : sender.key.identity);
    const auto participant = _participants.find(identity);
    return participant != _participants.end() &&
        participant->second.participantKey == sender.key;
}

void MeetingCoordinator::cancelInboundTransfersForParticipant(
    const livekit::ParticipantKey &participantKey) {
    auto session = _sessionRuntime;
    if (!session || !_sessionRunning.load(std::memory_order_acquire)) {
        return;
    }
    const uint64_t sessionGeneration = session->generation();

    asio::post(session->strand(), [this, session, sessionGeneration, participantKey]() {
        if (!session->acceptsDataOnStrand()) {
            return;
        }

        std::vector<QString> failedTransfers;
        auto &transfers = session->transfersOnStrand();
        for (auto it = transfers.begin(); it != transfers.end();) {
            if (it->second.senderKey == participantKey) {
                failedTransfers.push_back(it->first.uiTransferId());
                it = transfers.erase(it);
            } else {
                ++it;
            }
        }
        QMetaObject::invokeMethod(this, [this, sessionGeneration, participantKey,
                                         failedTransfers = std::move(failedTransfers)]() mutable {
            if (!isCurrentSessionGenerationOnUiThread(sessionGeneration)) {
                return;
            }
            QPointer<MeetingCoordinator> owner(this);
            for (auto it = _inboundTransferLedger.begin();
                 it != _inboundTransferLedger.end();) {
                if (it->second.first.nativeRoomGeneration == participantKey.native_room_generation &&
                    it->second.first.participantIncarnation == participantKey.incarnation &&
                    it->second.first.coordinatorSession == sessionGeneration) {
                    if (it->second.second) {
                        failedTransfers.erase(std::remove(failedTransfers.begin(),
                            failedTransfers.end(), it->first), failedTransfers.end());
                    } else if (std::find(failedTransfers.begin(), failedTransfers.end(), it->first) ==
                        failedTransfers.end()) {
                        failedTransfers.push_back(it->first);
                    }
                    it = _inboundTransferLedger.erase(it);
                } else {
                    ++it;
                }
            }
            for (const auto &transferId : failedTransfers) {
                emit owner->chatMediaReceivingFailed(transferId, QCoreApplication::translate("MeetingUI", "The sender has left the meeting"));
                if (!owner || !owner->isCurrentSessionGenerationOnUiThread(sessionGeneration)) return;
            }
        }, Qt::QueuedConnection);
    });
}

void MeetingCoordinator::handleDataReceivedOnSessionStrand(
    const std::shared_ptr<MeetingSessionRuntime> &session,
    const std::vector<uint8_t> &data,
    const livekit::SenderContext &sender) {
    session->assertOnStrand();
    if (!session->acceptsDataOnStrand()) {
        return;
    }
    const uint64_t sessionGeneration = session->generation();
    if (sender.origin == livekit::SenderOrigin::Unresolved ||
        (sender.origin != livekit::SenderOrigin::Server &&
         !livekit::IsParticipantTicketActive(sender.ticket, sender.key))) {
        return;
    }
    const std::string participantSid = sender.key.sid.empty()
        ? sender.transport_sid
        : sender.key.sid;
    const std::string participantIdentity = sender.key.identity.empty()
        ? sender.transport_identity
        : sender.key.identity;
    const QString participantName = QString::fromStdString(sender.display_name);

    openmeeting::meeting::NotifyMeetingData notify;
    if (!notify.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
        QString id = !participantIdentity.empty() ? QString::fromStdString(participantIdentity) : QString::fromStdString(participantSid);
        QString name = participantName;
        if (name.isEmpty() || name == id) {
            // Participant presentation state belongs to the Qt thread. The
            // Room callback already supplied the best available name snapshot.
            name = id;
        }
        auto &transfers = session->transfersOnStrand();
        const auto makeTransferKey = [&](const QString &wireTransferId) {
            return InboundTransferKey{
                sessionGeneration,
                sender.key.native_room_generation,
                sender.key.incarnation,
                wireTransferId};
        };

        // 1. 尝试解析为 JSON 消息协议 (chat_text, media_start, media_chunk)
        QJsonParseError jErr;
        QJsonDocument jDoc = QJsonDocument::fromJson(QByteArray::fromRawData(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size())), &jErr);
        if (jErr.error == QJsonParseError::NoError && jDoc.isObject()) {
            QJsonObject jObj = jDoc.object();
            QString omType = jObj.value("om_type").toString();

            // 1.1 文本聊天消息
            if (omType == "chat_text") {
                QString text = jObj.value("text").toString();
                int64_t seq = jObj.value("seq").toVariant().toLongLong();
                if (seq <= 0) seq = QDateTime::currentMSecsSinceEpoch() * 1000;
                QMetaObject::invokeMethod(this, [this, sessionGeneration, sender, id, name, text, seq]() {
                    if (!isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                        !isSenderContextCurrentOnUiThread(sender)) return;
                    emit chatMessageReceived(id, name, text, seq);
                }, Qt::QueuedConnection);
                return;
            }

            // 1.2 多媒体传输轻量预告包 (建立气泡占位)
            if (omType == "media_start") {
                QString transferId = jObj.value("transferId").toString();
                int64_t seq = jObj.value("seq").toVariant().toLongLong();
                if (seq <= 0) seq = QDateTime::currentMSecsSinceEpoch() * 1000;
                int totalChunks = jObj.value("totalChunks").toInt();
                QString mType = jObj.value("mediaType").toString();
                QString fName = jObj.value("fileName").toString();
                qint64 totalSize = jObj.value("totalSize").toVariant().toLongLong();
                const auto transferKey = makeTransferKey(transferId);
                const QString uiTransferId = transferKey.uiTransferId();

                auto &transfer = transfers[transferKey];
                transfer.mediaType = mType;
                transfer.fileName = fName;
                transfer.totalChunks = totalChunks;
                transfer.totalSize = totalSize;
                transfer.seq = seq;
                transfer.senderIdentity = id;
                transfer.senderName = name;
                transfer.senderKey = sender.key;
                transfer.senderTicket = sender.ticket;
                transfer.wireTransferId = transferId;
                transfer.lastActiveTimestamp = QDateTime::currentMSecsSinceEpoch();

                QMetaObject::invokeMethod(this, [this, sessionGeneration, sender, transferKey, uiTransferId,
                                                  id, name, mType, fName, totalSize, seq]() {
                    if (!isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                        !isSenderContextCurrentOnUiThread(sender)) return;
                    _inboundTransferLedger[uiTransferId] = {transferKey, false};
                    emit chatMediaReceivingStarted(uiTransferId, id, name, mType, fName, totalSize, seq);
                }, Qt::QueuedConnection);
                return;
            }

            // 1.3 多媒体数据分片
            if (omType == "media_chunk") {
                QString transferId = jObj.value("transferId").toString();
                int chunkIdx = jObj.value("chunkIndex").toInt();
                int totalChunks = jObj.value("totalChunks").toInt();
                QString mType = jObj.value("mediaType").toString();
                QString fName = jObj.value("fileName").toString();
                qint64 totalSize = jObj.value("totalSize").toVariant().toLongLong();
                int64_t seq = jObj.value("seq").toVariant().toLongLong();
                if (seq <= 0) seq = QDateTime::currentMSecsSinceEpoch() * 1000;
                QString chunkData = jObj.value("chunkData").toString();
                const auto transferKey = makeTransferKey(transferId);
                const QString uiTransferId = transferKey.uiTransferId();

                bool isFirstChunk = (transfers.find(transferKey) == transfers.end());
                auto &transfer = transfers[transferKey];
                if (isFirstChunk) {
                    transfer.mediaType = mType;
                    transfer.fileName = fName;
                    transfer.totalChunks = totalChunks;
                    transfer.totalSize = totalSize;
                    transfer.seq = seq;
                    transfer.senderIdentity = id;
                    transfer.senderName = name;
                    transfer.senderKey = sender.key;
                    transfer.senderTicket = sender.ticket;
                    transfer.wireTransferId = transferId;
                    transfer.lastActiveTimestamp = QDateTime::currentMSecsSinceEpoch();

                    QMetaObject::invokeMethod(this, [this, sessionGeneration, sender, transferKey, uiTransferId,
                                                      id, name, mType, fName, totalSize, seq]() {
                        if (!isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                            !isSenderContextCurrentOnUiThread(sender)) return;
                        _inboundTransferLedger[uiTransferId] = {transferKey, false};
                        emit chatMediaReceivingStarted(uiTransferId, id, name, mType, fName, totalSize, seq);
                    }, Qt::QueuedConnection);
                }

                if (transfer.senderKey != sender.key ||
                    !livekit::IsParticipantTicketActive(
                        transfer.senderTicket, transfer.senderKey)) {
                    transfers.erase(transferKey);
                    return;
                }

                transfer.lastActiveTimestamp = QDateTime::currentMSecsSinceEpoch();
                transfer.receivedChunks[chunkIdx] = chunkData;

                int progress = std::min(99, (static_cast<int>(transfer.receivedChunks.size()) * 100) / totalChunks);
                QMetaObject::invokeMethod(this, [this, sessionGeneration, sender, transferKey, uiTransferId, progress]() {
                    if (!isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                        !isSenderContextCurrentOnUiThread(sender)) return;
                    const auto ledger = _inboundTransferLedger.find(uiTransferId);
                    if (ledger == _inboundTransferLedger.end() ||
                        ledger->second.first < transferKey ||
                        transferKey < ledger->second.first ||
                        ledger->second.second) return;
                    emit chatMediaReceivingProgress(uiTransferId, progress);
                }, Qt::QueuedConnection);

                if (static_cast<int>(transfer.receivedChunks.size()) == totalChunks) {
                    QString fullBase64;
                    fullBase64.reserve(totalChunks * 10240);
                    for (int i = 0; i < totalChunks; ++i) {
                        fullBase64.append(transfer.receivedChunks[i]);
                    }
                    QByteArray completeData = QByteArray::fromBase64(fullBase64.toLatin1());
                    transfers.erase(transferKey);

                    QMetaObject::invokeMethod(this, [this, sessionGeneration, sender, transferKey, uiTransferId,
                                                      id, name, mType, fName, completeData]() {
                        QPointer<MeetingCoordinator> owner(this);
                        auto valid = [&](bool terminal) {
                            if (!owner ||
                                !owner->isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                                !owner->isSenderContextCurrentOnUiThread(sender)) return false;
                            const auto ledger = owner->_inboundTransferLedger.find(uiTransferId);
                            return ledger != owner->_inboundTransferLedger.end() &&
                                !(ledger->second.first < transferKey) &&
                                !(transferKey < ledger->second.first) &&
                                ledger->second.second == terminal;
                        };
                        if (!valid(false)) return;
                        emit owner->chatMediaReceivingProgress(uiTransferId, 100);
                        if (!valid(false)) return;
                        // Completion is the terminal linearization point. A
                        // synchronous leave/retire must not report failure too.
                        owner->_inboundTransferLedger.at(uiTransferId).second = true;
                        emit owner->chatMediaReceivingCompleted(uiTransferId, id, name, mType, fName, completeData);
                        if (!valid(true)) return;
                        owner->_inboundTransferLedger.erase(uiTransferId);
                        emit owner->chatMediaMessageReceived(id, name, mType, fName, completeData);
                    }, Qt::QueuedConnection);
                }
                return;
            }
        }

        // 2. 向下兼容：若不是 JSON 协议，作为普通文本聊天广播
        QString text = QString::fromUtf8(reinterpret_cast<const char *>(data.data()), static_cast<int>(data.size()));
        int64_t seq = QDateTime::currentMSecsSinceEpoch() * 1000;
        QMetaObject::invokeMethod(this, [this, sessionGeneration, sender, id, name, text, seq]() {
            if (!isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                !isSenderContextCurrentOnUiThread(sender)) return;
            emit chatMessageReceived(id, name, text, seq);
        }, Qt::QueuedConnection);
        return;
    }

    const QString &localUserId = session->localUserId();
    const bool isServerOrigin = sender.origin == livekit::SenderOrigin::Server;

    // 1. KickOff 踢出信令
    if (notify.has_kickoffmeetingdata()) {
        const auto &kick = notify.kickoffmeetingdata();
        QString targetUser = QString::fromStdString(kick.userid());
        if (targetUser == localUserId) {
            QString reason = QString::fromStdString(kick.reason());
            const QString safeReason = reason.isEmpty()
                ? QCoreApplication::translate("MeetingUI", "No additional details")
                : QString::fromStdString(
                    livekit::secure_log::OpaqueSummary("kick_reason"));
            int code = static_cast<int>(kick.reasoncode());
            QMetaObject::invokeMethod(
                this,
                [this, sessionGeneration, sender, reason, safeReason, code, isServerOrigin]() {
                QPointer<MeetingCoordinator> owner(this);
                const auto valid = [&] {
                    return owner && owner->isCurrentSessionGenerationOnUiThread(sessionGeneration) &&
                        owner->isSenderContextCurrentOnUiThread(sender);
                };
                if (!valid()) return;
                MeetingUI::LogToConsole(
                    MeetingUI::LogCategory::Participant,
                    "KICK_OFF",
                    QCoreApplication::translate("MeetingUI", "Removal signal received: %1 (code: %2)").arg(safeReason).arg(code));
                if (!valid()) return;
                if (code == static_cast<int>(openmeeting::meeting::KickOffReason::DuplicatedLogin)) {
                    // DuplicatedLogin 是全局账号事件，不能伪装成 LiveKit 的
                    // DuplicateIdentity 房间事件。只有没有参会者来源的服务端
                    // DataPacket 才拥有清理本地账号会话的权限。
                    if (!isServerOrigin) {
                        MeetingUI::LogToConsole(
                            MeetingUI::LogCategory::Error,
                            "UNTRUSTED_DUPLICATED_LOGIN",
                            "[Coordinator] Ignore DuplicatedLogin from a participant data message");
                        return;
                    }
                    // Trusted account notifications are consumed on their first
                    // Qt delivery, with the listener's authentication generation.
                    return;
                }
                emit owner->kickedOff(reason, code);
                if (!valid()) return;
                owner->leaveMeetingAsync(false);
                },
                Qt::QueuedConnection);
            return;
        }
    }

    // 2. StreamOperateData 远端流控信令 (开/关麦、开/关摄)
    if (notify.has_streamoperatedata()) {
        const auto &streamData = notify.streamoperatedata();
        QString opUser = QString::fromStdString(notify.operatoruserid());
        for (int i = 0; i < streamData.operation_size(); ++i) {
            const auto &op = streamData.operation(i);
            if (QString::fromStdString(op.userid()) == localUserId) {
                if (op.has_cameraonentry()) {
                    const bool camEnable = op.cameraonentry();
                    QMetaObject::invokeMethod(this, [this, sessionGeneration, sender, camEnable, opUser]() {
                        if (!isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                            !isSenderContextCurrentOnUiThread(sender)) return;
                        emit remoteMuteRequested(true, !camEnable, opUser);
                    }, Qt::QueuedConnection);
                }

                if (op.has_microphoneonentry()) {
                    const bool micEnable = op.microphoneonentry();
                    QMetaObject::invokeMethod(this, [this, sessionGeneration, sender, micEnable, opUser]() {
                        if (!isCurrentSessionGenerationOnUiThread(sessionGeneration) ||
                            !isSenderContextCurrentOnUiThread(sender)) return;
                        emit remoteMuteRequested(false, !micEnable, opUser);
                    }, Qt::QueuedConnection);
                }
            }
        }
    }

    // 3. MeetingHostData 主持人角色变更信令
    if (notify.has_meetinghostdata()) {
        const auto &hostData = notify.meetinghostdata();
        QString newHost = QString::fromStdString(hostData.userid());
        QString opNick = QString::fromStdString(hostData.operatornickname());
        QMetaObject::invokeMethod(this, [this, sessionGeneration, sender, newHost, opNick]() {
            QPointer<MeetingCoordinator> owner(this);
            const auto valid = [&] {
                return owner && owner->isCurrentSessionGenerationOnUiThread(sessionGeneration) &&
                    owner->isSenderContextCurrentOnUiThread(sender);
            };
            if (!valid()) return;
            _meetingDetail.hostUserId = newHost;
            for (auto &[id, p] : _participants) {
                p.isHost = (id == newHost);
            }
            updateParticipantListAndNotify();
            if (!valid()) return;
            emit owner->hostRoleChanged(newHost, opNick);
            if (!valid()) return;
            emit owner->meetingDetailUpdated(owner->_meetingDetail);
        }, Qt::QueuedConnection);
    }
}

std::shared_ptr<livekit::TextStreamWriter> MeetingCoordinator::createTextStreamWriter(
    const QString &topic,
    const std::map<std::string, std::string> &attributes,
    const QString &streamId,
    std::optional<size_t> totalSize,
    const QString &replyToId,
    const std::vector<std::string> &destinationIdentities) {
    if (!_room) return nullptr;
    return _room->CreateTextStreamWriter(
        topic.toStdString(), attributes, streamId.toStdString(),
        totalSize, replyToId.toStdString(), destinationIdentities);
}

std::shared_ptr<livekit::ByteStreamWriter> MeetingCoordinator::createByteStreamWriter(
    const QString &name,
    const QString &topic,
    const std::map<std::string, std::string> &attributes,
    const QString &streamId,
    std::optional<size_t> totalSize,
    const QString &mimeType,
    const std::vector<std::string> &destinationIdentities) {
    if (!_room) return nullptr;
    return _room->CreateByteStreamWriter(
        name.toStdString(), topic.toStdString(), attributes, streamId.toStdString(),
        totalSize, mimeType.toStdString(), destinationIdentities);
}

} // namespace OpenMeeting
