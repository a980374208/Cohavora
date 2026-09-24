#include "src/ui/telemetry_dialogs.h"

#include "src/telemetry/telemetry_report.h"
#include "src/ui/app_theme.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include <atomic>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

namespace MeetingUI {

QString LocalizeTelemetryDisplayText(const QString &value) {
	static const QHash<QString, QString> translations = {
		{QStringLiteral("VALID"), QCoreApplication::translate("TelemetryDisplay", "VALID")},
		{QStringLiteral("WARMING_UP"), QCoreApplication::translate("TelemetryDisplay", "WARMING_UP")},
		{QStringLiteral("NOT_EXPECTED"), QCoreApplication::translate("TelemetryDisplay", "NOT_EXPECTED")},
		{QStringLiteral("UNSUPPORTED"), QCoreApplication::translate("TelemetryDisplay", "UNSUPPORTED")},
		{QStringLiteral("TIMEOUT"), QCoreApplication::translate("TelemetryDisplay", "TIMEOUT")},
		{QStringLiteral("STALE"), QCoreApplication::translate("TelemetryDisplay", "STALE")},
		{QStringLiteral("INVALID"), QCoreApplication::translate("TelemetryDisplay", "INVALID")},
		{QStringLiteral("UNKNOWN"), QCoreApplication::translate("TelemetryDisplay", "UNKNOWN")},
		{QStringLiteral("COMPLETE"), QCoreApplication::translate("TelemetryDisplay", "COMPLETE")},
		{QStringLiteral("IN_PROGRESS"), QCoreApplication::translate("TelemetryDisplay", "IN_PROGRESS")},
		{QStringLiteral("true"), QCoreApplication::translate("TelemetryDisplay", "true")},
		{QStringLiteral("false"), QCoreApplication::translate("TelemetryDisplay", "false")},
		{QStringLiteral("admission"), QCoreApplication::translate("TelemetryDisplay", "admission")},
		{QStringLiteral("connect"), QCoreApplication::translate("TelemetryDisplay", "connect")},
		{QStringLiteral("startup"), QCoreApplication::translate("TelemetryDisplay", "startup")},
		{QStringLiteral("publish_batch"), QCoreApplication::translate("TelemetryDisplay", "publish_batch")},
		{QStringLiteral("publish_track"), QCoreApplication::translate("TelemetryDisplay", "publish_track")},
		{QStringLiteral("subscribe"), QCoreApplication::translate("TelemetryDisplay", "subscribe")},
		{QStringLiteral("unsubscribe"), QCoreApplication::translate("TelemetryDisplay", "unsubscribe")},
		{QStringLiteral("unpublish"), QCoreApplication::translate("TelemetryDisplay", "unpublish")},
		{QStringLiteral("disconnect"), QCoreApplication::translate("TelemetryDisplay", "disconnect")},
		{QStringLiteral("reconnect_episode"), QCoreApplication::translate("TelemetryDisplay", "reconnect_episode")},
		{QStringLiteral("reconnect_attempt"), QCoreApplication::translate("TelemetryDisplay", "reconnect_attempt")},
		{QStringLiteral("camera_device_switch"), QCoreApplication::translate("TelemetryDisplay", "camera_device_switch")},
		{QStringLiteral("microphone_device_switch"), QCoreApplication::translate("TelemetryDisplay", "microphone_device_switch")},
		{QStringLiteral("speaker_device_switch"), QCoreApplication::translate("TelemetryDisplay", "speaker_device_switch")},
		{QStringLiteral("unknown"), QCoreApplication::translate("TelemetryDisplay", "unknown")},
		{QStringLiteral("not_sampled"), QCoreApplication::translate("TelemetryDisplay", "not_sampled")},
		{QStringLiteral("stats_complete"), QCoreApplication::translate("TelemetryDisplay", "stats_complete")},
		{QStringLiteral("stats_partial_coverage"), QCoreApplication::translate("TelemetryDisplay", "stats_partial_coverage")},
		{QStringLiteral("stats_timeout"), QCoreApplication::translate("TelemetryDisplay", "stats_timeout")},
		{QStringLiteral("sample_stale"), QCoreApplication::translate("TelemetryDisplay", "sample_stale")},
		{QStringLiteral("no_peer_connection"), QCoreApplication::translate("TelemetryDisplay", "no_peer_connection")},
		{QStringLiteral("decoded_frame_received"), QCoreApplication::translate("TelemetryDisplay", "decoded_frame_received")},
		{QStringLiteral("waiting_for_decoded_frame"), QCoreApplication::translate("TelemetryDisplay", "waiting_for_decoded_frame")},
		{QStringLiteral("all_expected_bindings_decoded"), QCoreApplication::translate("TelemetryDisplay", "all_expected_bindings_decoded")},
		{QStringLiteral("no_remote_video_expected"), QCoreApplication::translate("TelemetryDisplay", "no_remote_video_expected")},
		{QStringLiteral("no_remote_video_binding"), QCoreApplication::translate("TelemetryDisplay", "no_remote_video_binding")},
		{QStringLiteral("no_inbound_video_stats"), QCoreApplication::translate("TelemetryDisplay", "no_inbound_video_stats")},
		{QStringLiteral("native_freeze_fields_missing"), QCoreApplication::translate("TelemetryDisplay", "native_freeze_fields_missing")},
		{QStringLiteral("pcm_received"), QCoreApplication::translate("TelemetryDisplay", "pcm_received")},
		{QStringLiteral("waiting_for_pcm"), QCoreApplication::translate("TelemetryDisplay", "waiting_for_pcm")},
		{QStringLiteral("all_expected_bindings_delivered_pcm"), QCoreApplication::translate("TelemetryDisplay", "all_expected_bindings_delivered_pcm")},
		{QStringLiteral("no_remote_audio_expected"), QCoreApplication::translate("TelemetryDisplay", "no_remote_audio_expected")},
		{QStringLiteral("no_remote_audio_binding"), QCoreApplication::translate("TelemetryDisplay", "no_remote_audio_binding")},
		{QStringLiteral("no_inbound_audio_stats"), QCoreApplication::translate("TelemetryDisplay", "no_inbound_audio_stats")},
		{QStringLiteral("audio_quality_fields_missing"), QCoreApplication::translate("TelemetryDisplay", "audio_quality_fields_missing")},
		{QStringLiteral("audio_delta_baseline_warming_up"), QCoreApplication::translate("TelemetryDisplay", "audio_delta_baseline_warming_up")},
		{QStringLiteral("concealment_window_valid"), QCoreApplication::translate("TelemetryDisplay", "concealment_window_valid")},
		{QStringLiteral("jitter_buffer_window_valid"), QCoreApplication::translate("TelemetryDisplay", "jitter_buffer_window_valid")},
		{QStringLiteral("time_stretch_window_valid"), QCoreApplication::translate("TelemetryDisplay", "time_stretch_window_valid")},
		{QStringLiteral("first_unique_frame_submitted"), QCoreApplication::translate("TelemetryDisplay", "first_unique_frame_submitted")},
		{QStringLiteral("visible_render_observed"), QCoreApplication::translate("TelemetryDisplay", "visible_render_observed")},
		{QStringLiteral("waiting_for_visible_submit"), QCoreApplication::translate("TelemetryDisplay", "waiting_for_visible_submit")},
		{QStringLiteral("all_expected_surfaces_submitted"), QCoreApplication::translate("TelemetryDisplay", "all_expected_surfaces_submitted")},
		{QStringLiteral("no_visible_render_expected"), QCoreApplication::translate("TelemetryDisplay", "no_visible_render_expected")},
		{QStringLiteral("no_expected_render_binding"), QCoreApplication::translate("TelemetryDisplay", "no_expected_render_binding")},
		{QStringLiteral("waiting_for_first_submit"), QCoreApplication::translate("TelemetryDisplay", "waiting_for_first_submit")},
		{QStringLiteral("static_content_not_classified"), QCoreApplication::translate("TelemetryDisplay", "static_content_not_classified")},
		{QStringLiteral("visible_render_stall_active"), QCoreApplication::translate("TelemetryDisplay", "visible_render_stall_active")},
		{QStringLiteral("render_stall_window_valid"), QCoreApplication::translate("TelemetryDisplay", "render_stall_window_valid")},
		{QStringLiteral("render_window_valid"), QCoreApplication::translate("TelemetryDisplay", "render_window_valid")},
		{QStringLiteral("no_reconnect_episode"), QCoreApplication::translate("TelemetryDisplay", "no_reconnect_episode")},
		{QStringLiteral("signaling_restored_waiting_for_video"), QCoreApplication::translate("TelemetryDisplay", "signaling_restored_waiting_for_video")},
		{QStringLiteral("signaling_restored_waiting_for_audio"), QCoreApplication::translate("TelemetryDisplay", "signaling_restored_waiting_for_audio")},
		{QStringLiteral("signaling_restored_waiting_for_render"), QCoreApplication::translate("TelemetryDisplay", "signaling_restored_waiting_for_render")},
		{QStringLiteral("decoded_video_stably_recovered"), QCoreApplication::translate("TelemetryDisplay", "decoded_video_stably_recovered")},
		{QStringLiteral("pcm_stably_recovered"), QCoreApplication::translate("TelemetryDisplay", "pcm_stably_recovered")},
		{QStringLiteral("visible_render_stably_recovered"), QCoreApplication::translate("TelemetryDisplay", "visible_render_stably_recovered")},
		{QStringLiteral("expectation_changed_during_recovery"), QCoreApplication::translate("TelemetryDisplay", "expectation_changed_during_recovery")},
		{QStringLiteral("no_remote_video_expected_at_outage"), QCoreApplication::translate("TelemetryDisplay", "no_remote_video_expected_at_outage")},
		{QStringLiteral("no_remote_audio_expected_at_outage"), QCoreApplication::translate("TelemetryDisplay", "no_remote_audio_expected_at_outage")},
		{QStringLiteral("no_visible_render_expected_at_outage"), QCoreApplication::translate("TelemetryDisplay", "no_visible_render_expected_at_outage")},
		{QStringLiteral("process_times_window_valid"), QCoreApplication::translate("TelemetryDisplay", "process_times_window_valid")},
		{QStringLiteral("process_memory_query_valid"), QCoreApplication::translate("TelemetryDisplay", "process_memory_query_valid")},
		{QStringLiteral("process_thread_count_valid"), QCoreApplication::translate("TelemetryDisplay", "process_thread_count_valid")},
		{QStringLiteral("process_handle_count_valid"), QCoreApplication::translate("TelemetryDisplay", "process_handle_count_valid")},
		{QStringLiteral("gpu_process_provider_not_configured"), QCoreApplication::translate("TelemetryDisplay", "gpu_process_provider_not_configured")},
		{QStringLiteral("resource_trend_not_started"), QCoreApplication::translate("TelemetryDisplay", "resource_trend_not_started")},
		{QStringLiteral("resource_trend_minimum_window_not_met"), QCoreApplication::translate("TelemetryDisplay", "resource_trend_minimum_window_not_met")},
		{QStringLiteral("post_stop_stable_window_not_observed"), QCoreApplication::translate("TelemetryDisplay", "post_stop_stable_window_not_observed")},
		{QStringLiteral("observed_sampler_snapshot_cost_valid"), QCoreApplication::translate("TelemetryDisplay", "observed_sampler_snapshot_cost_valid")},
		{QStringLiteral("controlled_enabled_disabled_run_not_executed"), QCoreApplication::translate("TelemetryDisplay", "controlled_enabled_disabled_run_not_executed")},
		{QStringLiteral("bounded_history_valid"), QCoreApplication::translate("TelemetryDisplay", "bounded_history_valid")},
		{QStringLiteral("history_store_not_installed"), QCoreApplication::translate("TelemetryDisplay", "history_store_not_installed")},
		{QStringLiteral("native_video_sink_onframe_entry"), QCoreApplication::translate("TelemetryDisplay", "native_video_sink_onframe_entry")},
		{QStringLiteral("rtc_inbound_video_sink"), QCoreApplication::translate("TelemetryDisplay", "rtc_inbound_video_sink")},
		{QStringLiteral("native_video_sink_stable_delivery"), QCoreApplication::translate("TelemetryDisplay", "native_video_sink_stable_delivery")},
		{QStringLiteral("native_audio_sink_ondata_entry"), QCoreApplication::translate("TelemetryDisplay", "native_audio_sink_ondata_entry")},
		{QStringLiteral("rtc_inbound_audio_stats"), QCoreApplication::translate("TelemetryDisplay", "rtc_inbound_audio_stats")},
		{QStringLiteral("native_audio_sink_stable_pcm_delivery"), QCoreApplication::translate("TelemetryDisplay", "native_audio_sink_stable_pcm_delivery")},
		{QStringLiteral("render_submit_not_registered"), QCoreApplication::translate("TelemetryDisplay", "render_submit_not_registered")},
		{QStringLiteral("visible_render_submit"), QCoreApplication::translate("TelemetryDisplay", "visible_render_submit")},
		{QStringLiteral("qt_cpu_paint"), QCoreApplication::translate("TelemetryDisplay", "qt_cpu_paint")},
		{QStringLiteral("local-safe-snapshot"), QCoreApplication::translate("TelemetryDisplay", "local-safe-snapshot")},
		{QStringLiteral("session_duration_in_progress"), QCoreApplication::translate("TelemetryDisplay", "session_duration_in_progress")},
		{QStringLiteral("session_duration_complete"), QCoreApplication::translate("TelemetryDisplay", "session_duration_complete")},
		{QStringLiteral("room_usable_duration_in_progress"), QCoreApplication::translate("TelemetryDisplay", "room_usable_duration_in_progress")},
		{QStringLiteral("room_usable_duration_complete"), QCoreApplication::translate("TelemetryDisplay", "room_usable_duration_complete")},
		{QStringLiteral("room_never_became_usable"), QCoreApplication::translate("TelemetryDisplay", "room_never_became_usable")},
		{QStringLiteral("waiting_for_connect_success"), QCoreApplication::translate("TelemetryDisplay", "waiting_for_connect_success")},
		{QStringLiteral("local_video_injection_timeout"), QCoreApplication::translate("TelemetryDisplay", "local_video_injection_timeout")},
		{QStringLiteral("waiting_for_local_video_injection"), QCoreApplication::translate("TelemetryDisplay", "waiting_for_local_video_injection")},
		{QStringLiteral("local_video_injection_observed"), QCoreApplication::translate("TelemetryDisplay", "local_video_injection_observed")},
		{QStringLiteral("local_video_ended_before_observation"), QCoreApplication::translate("TelemetryDisplay", "local_video_ended_before_observation")},
		{QStringLiteral("local_video_encode_timeout"), QCoreApplication::translate("TelemetryDisplay", "local_video_encode_timeout")},
		{QStringLiteral("outbound_video_mapping_unavailable"), QCoreApplication::translate("TelemetryDisplay", "outbound_video_mapping_unavailable")},
		{QStringLiteral("waiting_for_local_video_encode"), QCoreApplication::translate("TelemetryDisplay", "waiting_for_local_video_encode")},
		{QStringLiteral("local_video_encode_observed"), QCoreApplication::translate("TelemetryDisplay", "local_video_encode_observed")},
		{QStringLiteral("local_video_ended_before_encode_observation"), QCoreApplication::translate("TelemetryDisplay", "local_video_ended_before_encode_observation")},
		{QStringLiteral("no_local_video_expected"), QCoreApplication::translate("TelemetryDisplay", "no_local_video_expected")},
		{QStringLiteral("local_rtp_send_timeout"), QCoreApplication::translate("TelemetryDisplay", "local_rtp_send_timeout")},
		{QStringLiteral("outbound_rtp_mapping_unavailable"), QCoreApplication::translate("TelemetryDisplay", "outbound_rtp_mapping_unavailable")},
		{QStringLiteral("waiting_for_local_rtp_send"), QCoreApplication::translate("TelemetryDisplay", "waiting_for_local_rtp_send")},
		{QStringLiteral("local_rtp_send_observed"), QCoreApplication::translate("TelemetryDisplay", "local_rtp_send_observed")},
		{QStringLiteral("local_publication_ended_before_send_observation"), QCoreApplication::translate("TelemetryDisplay", "local_publication_ended_before_send_observation")},
		{QStringLiteral("no_local_publication_expected"), QCoreApplication::translate("TelemetryDisplay", "no_local_publication_expected")},
		{QStringLiteral("local_publication_no_media_timeout"), QCoreApplication::translate("TelemetryDisplay", "local_publication_no_media_timeout")},
		{QStringLiteral("local_publication_mapping_unavailable"), QCoreApplication::translate("TelemetryDisplay", "local_publication_mapping_unavailable")},
		{QStringLiteral("local_publication_media_warming_up"), QCoreApplication::translate("TelemetryDisplay", "local_publication_media_warming_up")},
		{QStringLiteral("all_expected_local_publications_sending"), QCoreApplication::translate("TelemetryDisplay", "all_expected_local_publications_sending")},
		{QStringLiteral("local_publications_ended_after_media_observed"), QCoreApplication::translate("TelemetryDisplay", "local_publications_ended_after_media_observed")},
		{QStringLiteral("local_publication_ended_before_media_observation"), QCoreApplication::translate("TelemetryDisplay", "local_publication_ended_before_media_observation")},
		{QStringLiteral("local_publications_not_expected_to_send"), QCoreApplication::translate("TelemetryDisplay", "local_publications_not_expected_to_send")},
		{QStringLiteral("no_local_publication"), QCoreApplication::translate("TelemetryDisplay", "no_local_publication")},
		{QStringLiteral("session_telemetry_lifetime"), QCoreApplication::translate("TelemetryDisplay", "session_telemetry_lifetime")},
		{QStringLiteral("connect_success_to_reconnect_or_disconnect"), QCoreApplication::translate("TelemetryDisplay", "connect_success_to_reconnect_or_disconnect")},
		{QStringLiteral("publish-media-v1"), QCoreApplication::translate("TelemetryDisplay", "publish-media-v1")},
		{QStringLiteral("rtc_local_video_source_onframe_submission"), QCoreApplication::translate("TelemetryDisplay", "rtc_local_video_source_onframe_submission")},
		{QStringLiteral("webrtc_outbound_rtp_frames_encoded_sample"), QCoreApplication::translate("TelemetryDisplay", "webrtc_outbound_rtp_frames_encoded_sample")},
		{QStringLiteral("webrtc_outbound_rtp_packets_sent_sample"), QCoreApplication::translate("TelemetryDisplay", "webrtc_outbound_rtp_packets_sent_sample")},
		{QStringLiteral("no_rtp_stats"), QCoreApplication::translate("TelemetryDisplay", "no_rtp_stats")},
		{QStringLiteral("recovery_counters_missing"), QCoreApplication::translate("TelemetryDisplay", "recovery_counters_missing")},
		{QStringLiteral("recovery_counter_baseline_warming_up"), QCoreApplication::translate("TelemetryDisplay", "recovery_counter_baseline_warming_up")},
		{QStringLiteral("recovery_counter_window_valid"), QCoreApplication::translate("TelemetryDisplay", "recovery_counter_window_valid")},
		{QStringLiteral("recovery_counter_partial_coverage"), QCoreApplication::translate("TelemetryDisplay", "recovery_counter_partial_coverage")},
		{QStringLiteral("network_category_no_streams"), QCoreApplication::translate("TelemetryDisplay", "network_category_no_streams")},
		{QStringLiteral("network_category_fields_missing"), QCoreApplication::translate("TelemetryDisplay", "network_category_fields_missing")},
		{QStringLiteral("network_category_baseline_warming_up"), QCoreApplication::translate("TelemetryDisplay", "network_category_baseline_warming_up")},
		{QStringLiteral("network_category_window_valid"), QCoreApplication::translate("TelemetryDisplay", "network_category_window_valid")},
		{QStringLiteral("network_category_partial_coverage"), QCoreApplication::translate("TelemetryDisplay", "network_category_partial_coverage")},
		{QStringLiteral("no_transport_stats"), QCoreApplication::translate("TelemetryDisplay", "no_transport_stats")},
		{QStringLiteral("selected_candidate_pair_id_missing"), QCoreApplication::translate("TelemetryDisplay", "selected_candidate_pair_id_missing")},
		{QStringLiteral("selected_candidate_pair_not_resolved"), QCoreApplication::translate("TelemetryDisplay", "selected_candidate_pair_not_resolved")},
		{QStringLiteral("selected_media_path_valid"), QCoreApplication::translate("TelemetryDisplay", "selected_media_path_valid")},
		{QStringLiteral("selected_media_path_partial_coverage"), QCoreApplication::translate("TelemetryDisplay", "selected_media_path_partial_coverage")},
		{QStringLiteral("no_outbound_video_stats"), QCoreApplication::translate("TelemetryDisplay", "no_outbound_video_stats")},
		{QStringLiteral("quality_limitation_fields_missing"), QCoreApplication::translate("TelemetryDisplay", "quality_limitation_fields_missing")},
		{QStringLiteral("quality_limitation_native_window_valid"), QCoreApplication::translate("TelemetryDisplay", "quality_limitation_native_window_valid")},
		{QStringLiteral("quality_limitation_native_partial_coverage"), QCoreApplication::translate("TelemetryDisplay", "quality_limitation_native_partial_coverage")},
		{QStringLiteral("no_local_device_running_intent"), QCoreApplication::translate("TelemetryDisplay", "no_local_device_running_intent")},
		{QStringLiteral("local_device_first_frame_warming_up"), QCoreApplication::translate("TelemetryDisplay", "local_device_first_frame_warming_up")},
		{QStringLiteral("local_device_unexpected_stop_active"), QCoreApplication::translate("TelemetryDisplay", "local_device_unexpected_stop_active")},
		{QStringLiteral("local_device_continuity_valid"), QCoreApplication::translate("TelemetryDisplay", "local_device_continuity_valid")},
		{QStringLiteral("local_device_probe_missing"), QCoreApplication::translate("TelemetryDisplay", "local_device_probe_missing")},
		{QStringLiteral("no_render_stage_samples"), QCoreApplication::translate("TelemetryDisplay", "no_render_stage_samples")},
		{QStringLiteral("render_cpu_stage_spans_valid"), QCoreApplication::translate("TelemetryDisplay", "render_cpu_stage_spans_valid")},
		{QStringLiteral("waiting_for_render_stage_sample"), QCoreApplication::translate("TelemetryDisplay", "waiting_for_render_stage_sample")},
		{QStringLiteral("gpu_timestamp_query_not_available"), QCoreApplication::translate("TelemetryDisplay", "gpu_timestamp_query_not_available")},
		{QStringLiteral("no_video_rtp_stats"), QCoreApplication::translate("TelemetryDisplay", "no_video_rtp_stats")},
		{QStringLiteral("video_frame_counters_missing"), QCoreApplication::translate("TelemetryDisplay", "video_frame_counters_missing")},
		{QStringLiteral("video_counter_baseline_warming_up"), QCoreApplication::translate("TelemetryDisplay", "video_counter_baseline_warming_up")},
		{QStringLiteral("video_pipeline_window_valid"), QCoreApplication::translate("TelemetryDisplay", "video_pipeline_window_valid")},
		{QStringLiteral("video_pipeline_partial_coverage"), QCoreApplication::translate("TelemetryDisplay", "video_pipeline_partial_coverage")},
		{QStringLiteral("codec_implementation_fields_missing"), QCoreApplication::translate("TelemetryDisplay", "codec_implementation_fields_missing")},
		{QStringLiteral("codec_id_implementation_and_layer_join_valid"), QCoreApplication::translate("TelemetryDisplay", "codec_id_implementation_and_layer_join_valid")},
		{QStringLiteral("codec_details_partial_coverage"), QCoreApplication::translate("TelemetryDisplay", "codec_details_partial_coverage")},
		{QStringLiteral("total_encode_decode_time_missing"), QCoreApplication::translate("TelemetryDisplay", "total_encode_decode_time_missing")},
		{QStringLiteral("processing_counter_baseline_warming_up"), QCoreApplication::translate("TelemetryDisplay", "processing_counter_baseline_warming_up")},
		{QStringLiteral("counter_delta_ms_per_frame_valid"), QCoreApplication::translate("TelemetryDisplay", "counter_delta_ms_per_frame_valid")},
		{QStringLiteral("same_clock_decode_to_submit_samples_valid"), QCoreApplication::translate("TelemetryDisplay", "same_clock_decode_to_submit_samples_valid")},
		{QStringLiteral("waiting_for_decode_to_submit_sample"), QCoreApplication::translate("TelemetryDisplay", "waiting_for_decode_to_submit_sample")},
		{QStringLiteral("no_render_binding"), QCoreApplication::translate("TelemetryDisplay", "no_render_binding")},
		{QStringLiteral("render_pipeline_not_sampled"), QCoreApplication::translate("TelemetryDisplay", "render_pipeline_not_sampled")},
		{QStringLiteral("render_pipeline_statistics_valid"), QCoreApplication::translate("TelemetryDisplay", "render_pipeline_statistics_valid")},
		{QStringLiteral("render_pipeline_statistics_with_typed_backend_failure"), QCoreApplication::translate("TelemetryDisplay", "render_pipeline_statistics_with_typed_backend_failure")},
		{QStringLiteral("bounded_latest_frame_router_observed"), QCoreApplication::translate("TelemetryDisplay", "bounded_latest_frame_router_observed")},
		{QStringLiteral("native_device_open_milestone_not_exposed"), QCoreApplication::translate("TelemetryDisplay", "native_device_open_milestone_not_exposed")},
		{QStringLiteral("os_device_change_provider_not_installed"), QCoreApplication::translate("TelemetryDisplay", "os_device_change_provider_not_installed")},
		{QStringLiteral("no_device_switch_operation"), QCoreApplication::translate("TelemetryDisplay", "no_device_switch_operation")},
		{QStringLiteral("operation_outcome_valid_native_failure_category_not_exposed"), QCoreApplication::translate("TelemetryDisplay", "operation_outcome_valid_native_failure_category_not_exposed")},
		{QStringLiteral("device_switch_operation_inflight"), QCoreApplication::translate("TelemetryDisplay", "device_switch_operation_inflight")},
		{QStringLiteral("device_switch_terminal_outcomes_valid"), QCoreApplication::translate("TelemetryDisplay", "device_switch_terminal_outcomes_valid")},
		{QStringLiteral("requested_source_waiting_for_samples"), QCoreApplication::translate("TelemetryDisplay", "requested_source_waiting_for_samples")},
		{QStringLiteral("requested_effective_state_and_actual_format_valid"), QCoreApplication::translate("TelemetryDisplay", "requested_effective_state_and_actual_format_valid")},
		{QStringLiteral("session_owned_binding_and_publication_counts_valid"), QCoreApplication::translate("TelemetryDisplay", "session_owned_binding_and_publication_counts_valid")},
		{QStringLiteral("history_export_queue_depth_not_exposed"), QCoreApplication::translate("TelemetryDisplay", "history_export_queue_depth_not_exposed")},
		{QStringLiteral("post_stop_sampler_not_owned_after_session_teardown"), QCoreApplication::translate("TelemetryDisplay", "post_stop_sampler_not_owned_after_session_teardown")},
		{QStringLiteral("rtc_rtp_stats_window"), QCoreApplication::translate("TelemetryDisplay", "rtc_rtp_stats_window")},
		{QStringLiteral("rtc_transport_selected_candidate_pair_id"), QCoreApplication::translate("TelemetryDisplay", "rtc_transport_selected_candidate_pair_id")},
		{QStringLiteral("rtc_outbound_video_quality_limitation"), QCoreApplication::translate("TelemetryDisplay", "rtc_outbound_video_quality_limitation")},
		{QStringLiteral("rtc_local_source_submission_probe"), QCoreApplication::translate("TelemetryDisplay", "rtc_local_source_submission_probe")},
		{QStringLiteral("render_cpu_submission_spans"), QCoreApplication::translate("TelemetryDisplay", "render_cpu_submission_spans")},
		{QStringLiteral("rtp_packets_including_retransmissions"), QCoreApplication::translate("TelemetryDisplay", "rtp_packets_including_retransmissions")},
		{QStringLiteral("device-continuity-v1"), QCoreApplication::translate("TelemetryDisplay", "device-continuity-v1")},
		{QStringLiteral("none"), QCoreApplication::translate("TelemetryDisplay", "none")},
		{QStringLiteral("cpu"), QCoreApplication::translate("TelemetryDisplay", "cpu")},
		{QStringLiteral("bandwidth"), QCoreApplication::translate("TelemetryDisplay", "bandwidth")},
		{QStringLiteral("other"), QCoreApplication::translate("TelemetryDisplay", "other")},
		{QStringLiteral("host"), QCoreApplication::translate("TelemetryDisplay", "host")},
		{QStringLiteral("srflx"), QCoreApplication::translate("TelemetryDisplay", "srflx")},
		{QStringLiteral("prflx"), QCoreApplication::translate("TelemetryDisplay", "prflx")},
		{QStringLiteral("relay"), QCoreApplication::translate("TelemetryDisplay", "relay")},
		{QStringLiteral("ethernet"), QCoreApplication::translate("TelemetryDisplay", "ethernet")},
		{QStringLiteral("wifi"), QCoreApplication::translate("TelemetryDisplay", "wifi")},
		{QStringLiteral("cellular"), QCoreApplication::translate("TelemetryDisplay", "cellular")},
		{QStringLiteral("vpn"), QCoreApplication::translate("TelemetryDisplay", "vpn")},
		{QStringLiteral("udp"), QCoreApplication::translate("TelemetryDisplay", "udp")},
		{QStringLiteral("tcp"), QCoreApplication::translate("TelemetryDisplay", "tcp")},
		{QStringLiteral("tls"), QCoreApplication::translate("TelemetryDisplay", "tls")},
		{QStringLiteral("active"), QCoreApplication::translate("TelemetryDisplay", "active")},
		{QStringLiteral("passive"), QCoreApplication::translate("TelemetryDisplay", "passive")},
		{QStringLiteral("so"), QCoreApplication::translate("TelemetryDisplay", "so")},
	};
	const auto found = translations.constFind(value);
	return found == translations.cend() ? value : *found;
}

QDialog *OpenPostMeetingTelemetryDialog(QWidget *parent) {
	const auto store = livekit::telemetry::InstalledTelemetryHistoryStore();
	auto *dialog = new QDialog(parent);
	dialog->setObjectName(QStringLiteral("telemetryPostMeetingDialog"));
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setWindowTitle(QCoreApplication::translate(
		"MeetingUI", "Telemetry reports on this device"));
	dialog->resize(760, 560);
	dialog->setMinimumSize(620, 440);
	AppTheme::setTone(*dialog, AppTheme::Tone::Light);
	auto *layout = new QVBoxLayout(dialog);
	layout->setContentsMargins(18, 18, 18, 18);
	layout->setSpacing(10);

	const auto records = store
		? store->CurrentRecords()
		: std::vector<livekit::telemetry::SafeTelemetryRecordPtr>{};
	auto *summary = new QTableWidget(dialog);
	summary->setObjectName(QStringLiteral("telemetryPostSummary"));
	summary->setColumnCount(2);
	summary->setHorizontalHeaderLabels({
		QCoreApplication::translate("MeetingUI", "Latest session"),
		QCoreApplication::translate("MeetingUI", "Value")});
	summary->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	summary->horizontalHeader()->setStretchLastSection(true);
	summary->verticalHeader()->setVisible(false);
	summary->setEditTriggers(QAbstractItemView::NoEditTriggers);
	const auto addSummary = [summary](const QString &name, const QString &value) {
		const auto row = summary->rowCount();
		summary->insertRow(row);
		summary->setItem(row, 0, new QTableWidgetItem(name));
		summary->setItem(row, 1, new QTableWidgetItem(value));
	};
	if (!records.empty() && records.back()) {
		const auto &record = *records.back();
		const auto &snapshot = record.snapshot;
		addSummary(QCoreApplication::translate("MeetingUI", "Observed at"),
			QDateTime::fromMSecsSinceEpoch(record.captured_utc_ms).toString(Qt::ISODate));
		addSummary(QCoreApplication::translate("MeetingUI", "Completion"),
			LocalizeTelemetryDisplayText(record.complete
				? QStringLiteral("COMPLETE") : QStringLiteral("IN_PROGRESS")));
		addSummary(QCoreApplication::translate("MeetingUI", "Availability / coverage"),
			QStringLiteral("%1 / %2%").arg(
				LocalizeTelemetryDisplayText(QString::fromLatin1(
					livekit::telemetry::AvailabilityName(snapshot.availability))))
				.arg(snapshot.coverage * 100.0, 0, 'f', 1));
		addSummary(QCoreApplication::translate("MeetingUI", "First decoded / visible video"),
			QStringLiteral("%1 ms / %2 ms").arg(
				snapshot.last_subscribe_to_first_decoded_ms)
				.arg(snapshot.last_subscribe_to_first_render_ms));
		addSummary(QCoreApplication::translate("MeetingUI", "Session / usable duration"),
			QStringLiteral("%1 ms / %2 ms").arg(snapshot.session_duration_ms)
				.arg(snapshot.usable_duration_ms));
		addSummary(QCoreApplication::translate("MeetingUI", "Local publish media"),
			QStringLiteral("%1 / no-media %2").arg(
				LocalizeTelemetryDisplayText(QString::fromLatin1(
					livekit::telemetry::AvailabilityName(
						snapshot.local_publish_media_availability))))
				.arg(snapshot.local_publish_no_media));
		addSummary(QCoreApplication::translate("MeetingUI", "Visible stalls"),
			QStringLiteral("%1 / %2 ms / %3").arg(snapshot.render_stall_count)
				.arg(snapshot.render_stall_duration_ms)
				.arg(QString::fromStdString(snapshot.render_stall_algorithm)));
		addSummary(QCoreApplication::translate("MeetingUI", "Operations terminal / inflight"),
			QStringLiteral("%1 / %2").arg(snapshot.operations_terminal)
				.arg(snapshot.operations_inflight));
	} else {
		addSummary(QCoreApplication::translate("MeetingUI", "Latest session"),
			QCoreApplication::translate("MeetingUI", "No local session summary"));
	}
	summary->setMaximumHeight(230);
	layout->addWidget(summary);

	const auto status = store ? store->Status() : nullptr;
	auto *state = new QLabel(status
		? QCoreApplication::translate(
			"MeetingUI", "%1 / %2 / MET-06 drops=%3, write failures=%4").arg(
			LocalizeTelemetryDisplayText(QString::fromLatin1(
				livekit::telemetry::AvailabilityName(status->availability))),
			LocalizeTelemetryDisplayText(QString::fromStdString(status->reason)),
			QString::number(status->queue_drops),
			QString::number(status->write_failures))
		: QStringLiteral("%1 / %2").arg(
			LocalizeTelemetryDisplayText(QStringLiteral("UNSUPPORTED")),
			LocalizeTelemetryDisplayText(QStringLiteral("history_store_not_installed"))), dialog);
	state->setObjectName(QStringLiteral("telemetryPostStatus"));
	state->setWordWrap(true);
	layout->addWidget(state);
	auto *historyEnabled = new QCheckBox(
		QCoreApplication::translate("MeetingUI", "Keep local telemetry history"), dialog);
	historyEnabled->setObjectName(QStringLiteral("telemetryHistoryEnabled"));
	historyEnabled->setChecked(status && status->history_enabled);
	historyEnabled->setEnabled(store != nullptr);
	layout->addWidget(historyEnabled);

	auto *reports = new QListWidget(dialog);
	reports->setObjectName(QStringLiteral("telemetryReports"));
	if (status) {
		for (const auto &entry : status->reports) {
			auto *item = new QListWidgetItem(
				QCoreApplication::translate("MeetingUI", "%1  %2 KiB  records=%3").arg(
					QDateTime::fromMSecsSinceEpoch(entry.created_utc_ms).toString(Qt::ISODate),
					QString::number(entry.size_bytes / 1024),
					QString::number(entry.record_count)), reports);
			item->setData(Qt::UserRole, QString::fromStdString(entry.record_id));
		}
	}
	layout->addWidget(reports, 1);

	auto *commands = new QHBoxLayout();
	auto *exportButton = new QPushButton(
		dialog->style()->standardIcon(QStyle::SP_DialogSaveButton),
		QCoreApplication::translate("MeetingUI", "Export latest"), dialog);
	exportButton->setObjectName(QStringLiteral("telemetryExport"));
	auto *clearButton = new QPushButton(
		dialog->style()->standardIcon(QStyle::SP_TrashIcon),
		QCoreApplication::translate("MeetingUI", "Clear selected"), dialog);
	clearButton->setObjectName(QStringLiteral("telemetryClearSelected"));
	clearButton->setEnabled(false);
	commands->addWidget(exportButton);
	commands->addWidget(clearButton);
	commands->addStretch();
	layout->addLayout(commands);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
	buttons->setObjectName(QStringLiteral("telemetryPostButtons"));
	buttons->button(QDialogButtonBox::Close)->setIcon(
		dialog->style()->standardIcon(QStyle::SP_DialogCloseButton));
	layout->addWidget(buttons);

	QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
	QObject::connect(reports, &QListWidget::currentItemChanged, clearButton,
		[clearButton](QListWidgetItem *current) { clearButton->setEnabled(current != nullptr); });
	QObject::connect(historyEnabled, &QCheckBox::toggled, dialog, [store](bool enabled) {
		if (store) store->SetHistoryEnabled(enabled);
		QSettings().setValue(QStringLiteral("telemetry/historyEnabled"), enabled);
	});
	QObject::connect(clearButton, &QPushButton::clicked, dialog,
		[store, reports, clearButton] {
			const auto *selected = reports->currentItem();
			if (!store || !selected) return;
			const auto id = selected->data(Qt::UserRole).toString().toStdString();
			clearButton->setEnabled(false);
			const QPointer<QListWidget> guard(reports);
			store->ClearReport(id, [guard, id](bool removed, std::string) {
				QMetaObject::invokeMethod(qApp, [guard, id, removed] {
					if (!guard || !removed) return;
					for (auto row = 0; row != guard->count(); ++row) {
						if (guard->item(row)->data(Qt::UserRole).toString().toStdString() == id) {
							delete guard->takeItem(row);
							break;
						}
					}
				}, Qt::QueuedConnection);
			});
		});
	QObject::connect(exportButton, &QPushButton::clicked, dialog, [store, dialog] {
		if (!store) return;
		const auto destination = QFileDialog::getExistingDirectory(
			dialog, QCoreApplication::translate("MeetingUI", "Export telemetry report"));
		if (destination.isEmpty()) return;
		const QPointer<QDialog> guard(dialog);
		auto cancelled = std::make_shared<std::atomic_bool>(false);
		auto *progress = new QProgressDialog(
			QCoreApplication::translate("MeetingUI", "Exporting telemetry report..."),
			QCoreApplication::translate("MeetingUI", "Cancel"), 0, 0, dialog);
		progress->setAttribute(Qt::WA_DeleteOnClose);
		progress->setWindowModality(Qt::WindowModal);
		progress->setMinimumDuration(0);
		progress->show();
		const QPointer<QProgressDialog> progressGuard(progress);
		QObject::connect(progress, &QProgressDialog::canceled, progress,
			[cancelled] { cancelled->store(true, std::memory_order_release); });
		if (!store->ExportCurrent(
				std::filesystem::path(destination.toStdWString()),
				[guard, progressGuard](livekit::telemetry::TelemetryExportResult result) {
					QMetaObject::invokeMethod(qApp, [guard, progressGuard, result = std::move(result)] {
						if (progressGuard) progressGuard->close();
						if (!guard) return;
						QMessageBox::information(
							guard,
							QCoreApplication::translate("MeetingUI", "Telemetry export"),
							result.success
								? QCoreApplication::translate("MeetingUI", "Report exported to %1")
									.arg(QString::fromStdWString(result.report_directory.wstring()))
								: QCoreApplication::translate("MeetingUI", "Export failed: %1")
									.arg(QString::fromStdString(result.reason)));
					}, Qt::QueuedConnection);
				}, cancelled)) {
			progress->close();
			QMessageBox::warning(
				dialog,
				QCoreApplication::translate("MeetingUI", "Telemetry export"),
				QCoreApplication::translate("MeetingUI", "Export queue is full"));
		}
	});
	AppTheme::makeDialogAdaptive(*dialog, QSize(760, 560));
	dialog->open();
	return dialog;
}

} // namespace MeetingUI
