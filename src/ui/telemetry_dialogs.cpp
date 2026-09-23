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
