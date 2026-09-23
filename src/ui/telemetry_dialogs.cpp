#include "src/ui/telemetry_dialogs.h"

#include "src/telemetry/telemetry_report.h"
#include "src/ui/app_theme.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
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
			record.complete ? QStringLiteral("COMPLETE") : QStringLiteral("IN_PROGRESS"));
		addSummary(QCoreApplication::translate("MeetingUI", "Availability / coverage"),
			QStringLiteral("%1 / %2%").arg(
				QString::fromLatin1(livekit::telemetry::AvailabilityName(snapshot.availability)))
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
		? QStringLiteral("%1 / %2 / MET-06 drops=%3 writes=%4").arg(
			QString::fromLatin1(livekit::telemetry::AvailabilityName(status->availability)),
			QString::fromStdString(status->reason),
			QString::number(status->queue_drops),
			QString::number(status->write_failures))
		: QStringLiteral("UNSUPPORTED / history_store_not_installed"), dialog);
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
				QStringLiteral("%1  %2 KiB  N=%3").arg(
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
