#include "src/platform/qt_path.h"
#include "src/core/session_shutdown_service.h"
#include "src/ui/telemetry_dialogs.h"
#include "src/ui/detail/meeting_room_helpers.h"
#include "src/ui/app_theme.h"
#include "src/telemetry/telemetry_report.h"
#include "directory_icon_provider.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDateTimeEdit>
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
#include <QtWidgets/QStyle>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace MeetingUI {
using ColorRole = AppTheme::ColorRole;
using detail::themeColor;

namespace {

class TelemetryTrendWidget final : public QWidget {
public:
	explicit TelemetryTrendWidget(
			std::vector<livekit::telemetry::SafeTelemetryRecordPtr> records,
			QWidget *parent = nullptr)
		: QWidget(parent)
		, _records(std::move(records)) {
		setMinimumHeight(180);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		setAccessibleName(QCoreApplication::translate(
			"MeetingUI", "CPU and private memory trend"));
		AppTheme::observeChanges(*this, [this] { update(); });
	}

protected:
	void paintEvent(QPaintEvent *) override {
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		const auto plot = rect().adjusted(44, 18, -18, -30);
		painter.setPen(themeColor(ColorRole::TextSecondary));
		painter.drawRect(plot);
		painter.drawText(8, 20, QCoreApplication::translate("MeetingUI", "CPU"));
		painter.drawText(
			8, height() - 8, QCoreApplication::translate("MeetingUI", "Memory"));
		if (_records.size() < 2 || plot.width() < 2 || plot.height() < 2) return;

		double maximumCpu = 1.0;
		std::uint64_t maximumMemory = 1;
		for (const auto &record : _records) {
			if (!record) continue;
			maximumCpu = std::max(maximumCpu, record->snapshot.process_cpu_percent);
			maximumMemory = std::max(maximumMemory, record->snapshot.private_bytes);
		}
		const auto pointAt = [&](std::size_t index, double ratio) {
			const auto x = plot.left() + qRound(
				static_cast<double>(plot.width()) * index /
				static_cast<double>(_records.size() - 1));
			const auto y = plot.bottom() - qRound(
				std::clamp(ratio, 0.0, 1.0) * plot.height());
			return QPoint(x, y);
		};
		QPolygon cpu;
		QPolygon memory;
		for (std::size_t i = 0; i != _records.size(); ++i) {
			const auto &snapshot = _records[i]->snapshot;
			cpu.push_back(pointAt(i,
				snapshot.process_cpu_percent < 0.0
					? 0.0 : snapshot.process_cpu_percent / maximumCpu));
			memory.push_back(pointAt(i,
				static_cast<double>(snapshot.private_bytes) /
				static_cast<double>(maximumMemory)));
		}
		painter.setPen(QPen(themeColor(ColorRole::Success), 2));
		painter.drawPolyline(cpu);
		painter.setPen(QPen(themeColor(ColorRole::Accent), 2));
		painter.drawPolyline(memory);
	}

private:
	std::vector<livekit::telemetry::SafeTelemetryRecordPtr> _records;
};

QTableWidget *MakeTelemetryTable(QWidget *parent, const QStringList &headers) {
	auto *table = new QTableWidget(parent);
	table->setColumnCount(headers.size());
	table->setHorizontalHeaderLabels(headers);
	table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
	table->horizontalHeader()->setStretchLastSection(true);
	table->verticalHeader()->setVisible(false);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setAlternatingRowColors(true);
	return table;
}

void AddTelemetryRow(QTableWidget *table, const QStringList &values) {
	const auto row = table->rowCount();
	table->insertRow(row);
	for (auto column = 0; column != values.size(); ++column) {
		auto *item = new QTableWidgetItem(values[column]);
		item->setToolTip(values[column]);
		table->setItem(row, column, item);
	}
}

QString TelemetryValue(const QVariantMap &snapshot, const char *key, const char *unit = "") {
	const auto value = snapshot.value(QString::fromLatin1(key));
	if (!value.isValid() || value.isNull()) return QStringLiteral("--");
	if ((value.type() == QVariant::Int || value.type() == QVariant::LongLong) &&
		value.toLongLong() < 0) {
		return QStringLiteral("--");
	}
	auto result = value.toString();
	if (*unit && result != QStringLiteral("--")) {
		result += QStringLiteral(" ") + QString::fromLatin1(unit);
	}
	return result;
}

QString TelemetryValueWithFallback(const QVariantMap &snapshot, const char *primaryKey, const char *fallbackKey, const char *unit = "") {
	const auto val = snapshot.value(QString::fromLatin1(primaryKey));
	if (val.isValid() && !val.isNull()) {
		if (!((val.type() == QVariant::Int || val.type() == QVariant::LongLong) && val.toLongLong() < 0)) {
			return TelemetryValue(snapshot, primaryKey, unit);
		}
	}
	return TelemetryValue(snapshot, fallbackKey, unit);
}

QString TelemetryRatioPercent(const QVariantMap &snapshot, const char *key) {
	const auto value = snapshot.value(QString::fromLatin1(key), -1.0).toDouble();
	return value < 0.0
		? QStringLiteral("--")
		: QString::number(value * 100.0, 'f', 2) + QStringLiteral(" %");
}

QString TelemetryRateMbps(const QVariantMap &snapshot, const char *key) {
	const auto value = snapshot.value(QString::fromLatin1(key), -1.0).toDouble();
	return value < 0.0
		? QStringLiteral("--")
		: QString::number(value / 1'000'000.0, 'f', 2) + QStringLiteral(" Mbps");
}

QString TelemetryDisplayVariant(const QVariant &value) {
	if (!value.isValid() || value.isNull()) return QStringLiteral("--");
	if (value.type() == QVariant::Int || value.type() == QVariant::LongLong) {
		return value.toLongLong() < 0 ? QStringLiteral("--") : value.toString();
	}
	if (value.type() == QVariant::Double) {
		return value.toDouble() < 0.0 ? QStringLiteral("--") : value.toString();
	}
	return LocalizeTelemetryDisplayText(value.toString());
}

QString TelemetryDisplayList(const QVariantMap &snapshot, const char *key) {
	const auto raw = snapshot.value(QString::fromLatin1(key)).toString();
	if (raw.isEmpty()) return QStringLiteral("--");
	auto values = raw.split(QLatin1Char(','), Qt::SkipEmptyParts);
	for (auto &value : values) value = LocalizeTelemetryDisplayText(value.trimmed());
	return values.join(QStringLiteral(", "));
}

#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
QString LinuxVideoEncoderDisplay(const QVariantMap &snapshot) {
	QStringList values;
	const auto backendName = [](const QString &backend) {
		if (backend == QStringLiteral("nvenc")) return QStringLiteral("NVIDIA NVENC");
		if (backend == QStringLiteral("qsv")) return QStringLiteral("Intel QSV");
		if (backend == QStringLiteral("vaapi")) return QStringLiteral("VAAPI");
		if (backend == QStringLiteral("amf")) return QStringLiteral("AMD AMF");
		if (backend == QStringLiteral("openh264")) return QStringLiteral("OpenH264");
		if (backend == QStringLiteral("libvpx")) return QStringLiteral("libvpx");
		if (backend == QStringLiteral("x265")) return QStringLiteral("x265");
		if (backend == QStringLiteral("auto")) return QCoreApplication::translate("MeetingUI", "Auto");
		if (backend == QStringLiteral("software")) return QCoreApplication::translate("MeetingUI", "Software");
		return QCoreApplication::translate("MeetingUI", "Unverified");
	};
	for (const auto &value : snapshot.value(QStringLiteral("linuxVideoEncoders")).toList()) {
		const auto encoder = value.toMap();
		const auto state = encoder.value(QStringLiteral("state")).toString();
		QString status;
		if (state == QStringLiteral("hardware_active")) status = QCoreApplication::translate("MeetingUI", "Hardware encoding active");
		else if (state == QStringLiteral("software_active")) status = QCoreApplication::translate("MeetingUI", "Software encoding active");
		else if (state == QStringLiteral("software_fallback")) status = QCoreApplication::translate("MeetingUI", "Software fallback active");
		else if (state == QStringLiteral("awaiting_output")) status = QCoreApplication::translate("MeetingUI", "Waiting for encoded output");
		else if (state == QStringLiteral("backend_mismatch")) status = QCoreApplication::translate("MeetingUI", "Observed encoder differs from selection");
		else if (state == QStringLiteral("mixed_output")) status = QCoreApplication::translate("MeetingUI", "Encoding streams use different backends");
		else if (state == QStringLiteral("partial_output")) status = QCoreApplication::translate("MeetingUI", "Some encoding streams are awaiting output");
		else status = QCoreApplication::translate("MeetingUI", "Encoder output unverified");
		const auto source_kind = encoder.value(QStringLiteral("source")).toString();
		const auto source = source_kind == QStringLiteral("camera")
			? QCoreApplication::translate("MeetingUI", "Camera") :
			(source_kind == QStringLiteral("screen") ? QCoreApplication::translate("MeetingUI", "Screen sharing") :
				QCoreApplication::translate("MeetingUI", "Video"));
		values.push_back(QCoreApplication::translate("MeetingUI", "%1: %2; selected %3; observed %4")
			.arg(source, status, backendName(encoder.value(QStringLiteral("requestedBackend")).toString()),
				backendName(encoder.value(QStringLiteral("encoder")).toString())));
	}
	if (snapshot.value(QStringLiteral("linuxVideoEncodersOverflow")).toBool()) {
		values.push_back(QCoreApplication::translate("MeetingUI", "Additional encoder states unavailable"));
	}
	return values.isEmpty() ? QStringLiteral("--") : values.join(QStringLiteral("; "));
}
#endif

bool IsSafeTelemetryDisplayEntry(const QString &key, const QVariant &value) {
	const auto normalizedKey = key.toCaseFolded();
	static const auto sensitiveKeyParts = std::array{
		QStringLiteral("url"), QStringLiteral("token"), QStringLiteral("ice"),
		QStringLiteral("sdp"), QStringLiteral("path"), QStringLiteral("identity"),
		QStringLiteral("password"), QStringLiteral("secret"),
		QStringLiteral("credential")};
	for (const auto &part : sensitiveKeyParts) {
		if (normalizedKey.contains(part)) return false;
	}
	const auto text = value.toString().trimmed();
	const auto folded = text.toCaseFolded();
	if (folded.contains(QStringLiteral("://")) ||
		folded.startsWith(QStringLiteral("candidate:")) ||
		folded.startsWith(QStringLiteral("v=0")) ||
		folded.contains(QStringLiteral("bearer "))) {
		return false;
	}
	return text.size() < 3 || text[1] != QLatin1Char(':') ||
		(text[2] != QLatin1Char('\\') && text[2] != QLatin1Char('/'));
}

void ShowTelemetryExport(QWidget *parent, std::string record_id = {},
                         std::int64_t first_utc_ms = 0,
                         std::int64_t last_utc_ms = 0) {
	const auto store = livekit::telemetry::InstalledTelemetryHistoryStore();
	if (!store) return;
	DirectoryIconProvider icons(*QApplication::style());
	QFileDialog dialog(parent, QCoreApplication::translate("MeetingUI", "Export telemetry report"));
	dialog.setObjectName(QStringLiteral("telemetryExportDirectory"));
	dialog.setOption(QFileDialog::DontUseNativeDialog);
	dialog.setOption(QFileDialog::ShowDirsOnly);
	dialog.setFileMode(QFileDialog::Directory);
	dialog.setIconProvider(&icons);
	if (auto *buttons = dialog.findChild<QDialogButtonBox *>()) {
		if (auto *accept = buttons->button(QDialogButtonBox::Open))
			accept->setObjectName(QStringLiteral("telemetryExportAccept"));
		if (auto *cancel = buttons->button(QDialogButtonBox::Cancel))
			cancel->setObjectName(QStringLiteral("telemetryExportCancel"));
	}
	AppTheme::setTone(dialog, AppTheme::Tone::Light);
	AppTheme::styleChoiceControls(dialog, AppTheme::Tone::Light);
	AppTheme::makeDialogAdaptive(dialog, {760, 520});
	if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
	const auto directory = dialog.selectedFiles().front();
	const QPointer<QWidget> guard(parent);
	auto cancelled = std::make_shared<std::atomic_bool>(false);
	auto *progress = new QProgressDialog(
		QCoreApplication::translate("MeetingUI", "Exporting telemetry report..."),
		QCoreApplication::translate("MeetingUI", "Cancel"), 0, 0, parent);
	progress->setAttribute(Qt::WA_DeleteOnClose);
	progress->setWindowModality(Qt::WindowModal);
	progress->setMinimumDuration(0);
	progress->show();
	const QPointer<QProgressDialog> progressGuard(progress);
	QObject::connect(progress, &QProgressDialog::canceled, progress,
		[cancelled] { cancelled->store(true, std::memory_order_release); });
	const auto destination = cohavora::platform::PathFromQString(directory);
	const auto dispatcher = OpenMeeting::detail::QtDispatchEndpoint::Create();
	auto completion = [dispatcher, guard, progressGuard](livekit::telemetry::TelemetryExportResult result) {
			if (dispatcher) dispatcher->Post( [guard, progressGuard, result = std::move(result)] {
				if (progressGuard) progressGuard->close();
				if (!guard) return;
				QMessageBox message(guard);
				message.setObjectName(QStringLiteral("meetingTelemetryExportResult"));
				message.setWindowTitle(QCoreApplication::translate(
					"MeetingUI", "Telemetry export"));
				message.setIcon(result.success
					? QMessageBox::Information : QMessageBox::Warning);
				message.setText(result.success
					? QCoreApplication::translate("MeetingUI", "Report exported")
					: QCoreApplication::translate("MeetingUI", "Export failed: %1")
						.arg(QString::fromStdString(result.reason)));
				if (result.success) {
					message.setInformativeText(cohavora::platform::QStringFromPath(result.report_directory));
				}
				AppTheme::setTone(message, AppTheme::Tone::Dark);
				message.setStandardButtons(QMessageBox::Ok);
				if (auto *dismiss = message.button(QMessageBox::Ok))
					dismiss->setObjectName(QStringLiteral("meetingTelemetryExportDismiss"));
				message.exec();
			});
		};
	if (record_id.empty())
		store->ExportCurrent(destination, std::move(completion), cancelled);
	else
		store->ExportReport(std::move(record_id), destination,
			std::move(completion), cancelled, first_utc_ms, last_utc_ms);
}

} // namespace

QDialog *OpenTelemetryDetailsDialog(QWidget *parent, const QVariantMap &snapshot) {
	auto *dialog = new QDialog(parent);
	dialog->setObjectName(QStringLiteral("telemetryDetailsDialog"));
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	AppTheme::configureModelessWindow(*dialog);
	dialog->setWindowFlag(Qt::WindowMinimizeButtonHint, false);
	dialog->setWindowTitle(QCoreApplication::translate("MeetingUI", "Meeting telemetry"));
	dialog->resize(900, 640);
	dialog->setMinimumSize(680, 480);
	AppTheme::setTone(*dialog, AppTheme::Tone::Dark);

	auto *layout = new QVBoxLayout(dialog);
	layout->setContentsMargins(16, 16, 16, 16);
	layout->setSpacing(12);
	auto *tabs = new QTabWidget(dialog);
	tabs->setObjectName(QStringLiteral("telemetryTabs"));
	layout->addWidget(tabs, 1);

	const auto addSummaryPage = [&](const QString &title,
			const std::vector<std::array<QString, 4>> &rows) {
		auto *table = MakeTelemetryTable(tabs, {
			QCoreApplication::translate("MeetingUI", "Metric"),
			QCoreApplication::translate("MeetingUI", "Value"),
			QCoreApplication::translate("MeetingUI", "Availability"),
			QCoreApplication::translate("MeetingUI", "Reason / boundary")});
		for (const auto &row : rows) AddTelemetryRow(table, {row[0], row[1], row[2], row[3]});
		tabs->addTab(table, title);
	};

	addSummaryPage(QCoreApplication::translate("MeetingUI", "Overview"), {
		{QCoreApplication::translate("MeetingUI", "Session"),
		 TelemetryValue(snapshot, "sessionGeneration"),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("availability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("reason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Coverage"),
		 QString::number(snapshot.value(QStringLiteral("coverage")).toDouble() * 100.0, 'f', 1) + QStringLiteral(" %"),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("availability")).toString()), QString()},
		{QCoreApplication::translate("MeetingUI", "Session / usable duration"),
		 TelemetryValue(snapshot, "sessionDurationMs", "ms") + QStringLiteral(" / ") +
		 TelemetryValue(snapshot, "usableDurationMs", "ms"),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("usableDurationAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("usableDurationReason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Admission to usable meeting"),
		 TelemetryValue(snapshot, "admissionToUsableMs", "ms"),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("admissionToUsableAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("admissionToUsableReason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Schema / definition"),
		 QStringLiteral("%1 / %2").arg(
			snapshot.value(QStringLiteral("schemaVersion")).toString(),
			snapshot.value(QStringLiteral("definitionVersion")).toString()),
		 LocalizeTelemetryDisplayText(QStringLiteral("VALID")),
		 LocalizeTelemetryDisplayText(QStringLiteral("local-safe-snapshot"))},
		{QCoreApplication::translate("MeetingUI", "First decoded video"),
		 TelemetryValueWithFallback(snapshot, "firstSubscribeToFirstDecodedMs", "lastSubscribeToFirstDecodedMs", "ms"),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("firstVideoAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("firstVideoMeasurementPoint")).toString())},
		{QCoreApplication::translate("MeetingUI", "First visible render"),
		 QCoreApplication::translate("MeetingUI", "admission %1 / subscription %2")
			 .arg(TelemetryValueWithFallback(snapshot, "firstAdmissionToFirstRenderMs", "lastAdmissionToFirstRenderMs", "ms"),
				  TelemetryValueWithFallback(snapshot, "firstSubscribeToFirstRenderMs", "lastSubscribeToFirstRenderMs", "ms")),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("renderFirstFrameAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("renderMeasurementPoint")).toString())},
		{QCoreApplication::translate("MeetingUI", "First remote PCM"),
		 TelemetryValueWithFallback(snapshot, "firstSubscribeToFirstPcmMs", "lastSubscribeToFirstPcmMs", "ms"),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("firstAudioAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("firstAudioMeasurementPoint")).toString())},
		{QCoreApplication::translate("MeetingUI", "Active operations"),
		 TelemetryValue(snapshot, "operationsInflight"),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("availability")).toString()), QString()},
	});

	addSummaryPage(QCoreApplication::translate("MeetingUI", "Media QoE"), {
		{QCoreApplication::translate("MeetingUI", "Local publish media"),
		 TelemetryValue(snapshot, "localPublishNoMedia"),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("localPublishMediaAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("localPublishMediaReason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Publish to injection / encode / send"),
		 TelemetryValue(snapshot, "lastPublishToVideoInjectionMs", "ms") + QStringLiteral(" / ") +
		 TelemetryValue(snapshot, "lastPublishToVideoEncodeMs", "ms") + QStringLiteral(" / ") +
		 TelemetryValue(snapshot, "lastPublishToRtpSendMs", "ms"),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("localRtpSendAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("localRtpSendReason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Subscribed media delivery"),
		 QCoreApplication::translate("MeetingUI", "%1 delivered / %2 expected; %3 timeout")
			 .arg(TelemetryValue(snapshot, "deliveredRemoteSubscriptions"),
				  TelemetryValue(snapshot, "expectedRemoteSubscriptions"),
				  TelemetryValue(snapshot, "remoteSubscriptionNoMedia")),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("subscriptionMediaAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("subscriptionMediaReason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Native video freezes"),
		 TelemetryValue(snapshot, "nativeVideoFreezeCount"),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("nativeVideoFreezeAvailability")).toString()),
		 TelemetryValue(snapshot, "nativeVideoFreezeDurationMs", "ms")},
		{QCoreApplication::translate("MeetingUI", "Video quality limitation"),
		 TelemetryDisplayList(snapshot, "videoQualityLimitationCurrent"),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("videoQualityLimitationAvailability")).toString()),
		 QCoreApplication::translate("MeetingUI", "CPU %1 ms / bandwidth %2 ms")
			 .arg(TelemetryValue(snapshot, "windowVideoQualityCpuDurationMs"),
				  TelemetryValue(snapshot, "windowVideoQualityBandwidthDurationMs"))},
		{QCoreApplication::translate("MeetingUI", "Video pipeline frames"),
		 QCoreApplication::translate("MeetingUI", "receive %1 / decode %2 / drop %3; encode %4 / send %5")
			 .arg(TelemetryValue(snapshot, "windowInboundVideoFramesReceived"),
				  TelemetryValue(snapshot, "windowInboundVideoFramesDecoded"),
				  TelemetryValue(snapshot, "windowInboundVideoFramesDropped"),
				  TelemetryValue(snapshot, "windowOutboundVideoFramesEncoded"),
				  TelemetryValue(snapshot, "windowOutboundVideoFramesSent")),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("videoPipelineAvailability")).toString()),
		 QCoreApplication::translate("MeetingUI", "Inbound drop %1")
			 .arg(TelemetryRatioPercent(snapshot, "inboundVideoFrameDropRatio"))},
		{QCoreApplication::translate("MeetingUI", "Video codec / implementation / layers"),
		 TelemetryDisplayList(snapshot, "inboundVideoCodecs") + QStringLiteral(" / ") +
			 TelemetryDisplayList(snapshot, "outboundVideoCodecs"),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("videoCodecAvailability")).toString()),
		 QCoreApplication::translate("MeetingUI", "decoder %1; encoder %2; layers %3")
			 .arg(TelemetryDisplayList(snapshot, "decoderImplementations"),
				  TelemetryDisplayList(snapshot, "encoderImplementations"),
				  TelemetryDisplayList(snapshot, "outboundVideoLayers"))},
		{QCoreApplication::translate("MeetingUI", "Video publish requested / effective / observed"),
		 TelemetryDisplayList(snapshot, "videoPublishRequestedCodecs") + QStringLiteral(" / ") +
			 TelemetryDisplayList(snapshot, "videoPublishEffectiveCodecs") + QStringLiteral(" / ") +
			 TelemetryDisplayList(snapshot, "videoPublishObservedCodecs"),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("videoPublishPlanAvailability")).toString()),
		 QCoreApplication::translate("MeetingUI", "source %1; mode %2; fallback %3")
			 .arg(TelemetryDisplayList(snapshot, "videoPublishSources"),
				  TelemetryDisplayList(snapshot, "videoPublishModes"),
				  TelemetryDisplayList(snapshot, "videoPublishFallbackReasons"))},
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
		{QCoreApplication::translate("MeetingUI", "Linux video encoder"),
		 LinuxVideoEncoderDisplay(snapshot), QString(),
		 QCoreApplication::translate("MeetingUI", "Status follows encoded frames from the current publication.")},
#endif
		{QCoreApplication::translate("MeetingUI", "Video processing average"),
		 QCoreApplication::translate("MeetingUI", "decode %1 ms/frame / encode %2 ms/frame")
			 .arg(TelemetryValue(snapshot, "videoDecodeMsPerFrame"),
				  TelemetryValue(snapshot, "videoEncodeMsPerFrame")),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("videoProcessingAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("videoProcessingReason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Visible render stalls"),
		 TelemetryValue(snapshot, "renderStallCount"),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("renderStallAvailability")).toString()),
		 TelemetryValue(snapshot, "renderStallDurationMs", "ms") + QStringLiteral(" / ") +
		 snapshot.value(QStringLiteral("renderStallAlgorithm")).toString()},
		{QCoreApplication::translate("MeetingUI", "Render CPU stages"),
		 QCoreApplication::translate("MeetingUI", "convert %1 / upload %2 / draw %3 / present block %4")
			 .arg(TelemetryValue(snapshot, "renderConvertMaxUs", "us"),
				  TelemetryValue(snapshot, "renderUploadMaxUs", "us"),
				  TelemetryValue(snapshot, "renderDrawMaxUs", "us"),
				  TelemetryValue(snapshot, "renderPresentBlockMaxUs", "us")),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("renderStageAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("renderGpuExecutionReason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Render cadence / frame age"),
		 QCoreApplication::translate("MeetingUI", "%1 fps; P50/P95/P99 %2 / %3 / %4 ms; age %5 / %6 ms")
			 .arg(TelemetryValue(snapshot, "renderSubmitFps"),
				  TelemetryValue(snapshot, "renderIntervalP50Ms"),
				  TelemetryValue(snapshot, "renderIntervalP95Ms"),
				  TelemetryValue(snapshot, "renderIntervalP99Ms"),
				  TelemetryValue(snapshot, "renderAverageFrameAgeMs"),
				  TelemetryValue(snapshot, "renderMaximumFrameAgeMs")),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("renderFrameAgeAvailability")).toString()),
		 QCoreApplication::translate("MeetingUI", "target %1 ms; visible %2, hidden %3, minimized %4")
			 .arg(TelemetryValue(snapshot, "renderTargetIntervalMs"),
				  TelemetryValue(snapshot, "renderExpectedBindings"),
				  TelemetryValue(snapshot, "renderHiddenBindings"),
				  TelemetryValue(snapshot, "renderMinimizedBindings"))},
		{QCoreApplication::translate("MeetingUI", "Render backend / router drops"),
		 TelemetryDisplayList(snapshot, "renderRequestedBackend") + QStringLiteral(" -> ") +
			 TelemetryDisplayList(snapshot, "renderActualBackend"),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("renderPipelineAvailability")).toString()),
		 QCoreApplication::translate("MeetingUI", "replace %1; capacity %2; invalid %3; conversion %4; fallback %5")
			 .arg(TelemetryValue(snapshot, "renderRouterReplaced"),
				  TelemetryValue(snapshot, "renderRouterDroppedCapacity"),
				  TelemetryValue(snapshot, "renderRouterDroppedInvalid"),
				  TelemetryValue(snapshot, "renderQtConversionFailures"),
				  TelemetryDisplayList(snapshot, "renderFallbackReason"))},
		{QCoreApplication::translate("MeetingUI", "Audio concealment"),
		 TelemetryRatioPercent(snapshot, "audioConcealedRatio"),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("audioConcealmentAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("audioConcealmentReason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Jitter buffer delay"),
		 TelemetryValue(snapshot, "audioJitterBufferDelayMs", "ms"),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("audioJitterBufferAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("audioJitterBufferReason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Reconnect video / audio / render"),
		 TelemetryValue(snapshot, "lastReconnectStableVideoMs", "ms") + QStringLiteral(" / ") +
		 TelemetryValue(snapshot, "lastReconnectStableAudioMs", "ms") + QStringLiteral(" / ") +
		 TelemetryValue(snapshot, "lastReconnectStableRenderMs", "ms"),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("reconnectRenderAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("reconnectRenderReason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Reconnect episode density"),
		 QCoreApplication::translate("MeetingUI", "%1 episodes / %2 per hour")
			 .arg(TelemetryValue(snapshot, "reconnectEpisodes"),
				  TelemetryValue(snapshot, "reconnectEpisodesPerHour")),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("reconnectDensityAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("reconnectDensityReason")).toString())},
	});
	tabs->widget(0)->setObjectName(QStringLiteral("telemetryOverview"));
	tabs->widget(1)->setObjectName(QStringLiteral("telemetryMediaQoe"));

	addSummaryPage(QCoreApplication::translate("MeetingUI", "Network and devices"), {
		{QCoreApplication::translate("MeetingUI", "RTP media bitrate inbound / outbound"),
		 TelemetryRateMbps(snapshot, "inboundRtpBitrateBps") +
			 QStringLiteral(" / ") +
			 TelemetryRateMbps(snapshot, "outboundRtpBitrateBps"),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("inboundRtpTrafficAvailability")).toString()) +
			 QStringLiteral(" / ") + LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("outboundRtpTrafficAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("inboundRtpTrafficReason")).toString()) +
			 QStringLiteral(" / ") + LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("outboundRtpTrafficReason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Inbound RTP loss / jitter"),
		 TelemetryRatioPercent(snapshot, "inboundPacketLossRatio") +
			 QStringLiteral(" / ") + TelemetryValue(snapshot, "inboundJitterMaxMs", "ms"),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("inboundPacketLossAvailability")).toString()) +
			 QStringLiteral(" / ") + LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("inboundJitterAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("inboundPacketLossReason")).toString()) +
			 QStringLiteral(" / ") + LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("inboundJitterReason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Remote RTCP RTT / loss"),
		 TelemetryValue(snapshot, "remoteRtcpCurrentRttMaxMs", "ms") +
			 QStringLiteral(" / ") +
			 TelemetryValue(snapshot, "remoteRtcpWindowAverageRttMs", "ms") +
			 QStringLiteral(" / ") +
			 TelemetryRatioPercent(snapshot, "remoteRtcpFractionLostMax"),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("remoteRtcpAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("remoteRtcpReason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Retransmission inbound / outbound"),
		 TelemetryRatioPercent(snapshot, "inboundRetransmittedPacketRatio") +
			 QStringLiteral(" / ") +
			 TelemetryRatioPercent(snapshot, "outboundRetransmittedPacketRatio"),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("networkRecoveryAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("networkRetransmitRatioDenominator")).toString())},
		{QCoreApplication::translate("MeetingUI", "FEC / NACK / PLI / FIR window"),
		 QStringLiteral("%1 / %2 / %3 / %4").arg(
			 TelemetryValue(snapshot, "windowInboundFecPackets"),
			 TelemetryValue(snapshot, "inboundNackCount"),
			 TelemetryValue(snapshot, "inboundPliCount"),
			 TelemetryValue(snapshot, "inboundFirCount")),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("networkRecoveryAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("networkRecoveryReason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Selected media path"),
		 QStringLiteral("%1 / %2 / %3").arg(
			 TelemetryDisplayList(snapshot, "localCandidateTypes"),
			 TelemetryDisplayList(snapshot, "localNetworkTypes"),
			 TelemetryDisplayList(snapshot, "mediaProtocols")),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("mediaPathAvailability")).toString()),
		 QCoreApplication::translate("MeetingUI", "switches %1; signaling transport excluded")
			 .arg(TelemetryValue(snapshot, "mediaPathSwitches"))},
		{QCoreApplication::translate("MeetingUI", "TURN relay / TCP mode"),
		 TelemetryDisplayList(snapshot, "relayProtocols") + QStringLiteral(" / ") +
			 TelemetryDisplayList(snapshot, "tcpTypes"),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("mediaPathAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("mediaPathReason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Selected path RTT / available bandwidth"),
		 TelemetryValue(snapshot, "mediaPathRttMaxMs", "ms") + QStringLiteral(" / ") +
			 TelemetryRateMbps(snapshot, "mediaAvailableOutgoingBitrateBps") +
			 QStringLiteral(" / ") +
			 TelemetryRateMbps(snapshot, "mediaAvailableIncomingBitrateBps"),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("mediaPathRttAvailability")).toString()) +
			 QStringLiteral(" / ") + LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("mediaBandwidthAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("mediaPathRttReason")).toString()) +
			 QStringLiteral(" / ") + LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("mediaBandwidthReason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Transport traffic window"),
		 QCoreApplication::translate("MeetingUI", "send %1 B / receive %2 B; %3 / %4 packets")
			 .arg(TelemetryValue(snapshot, "windowTransportBytesSent"),
				  TelemetryValue(snapshot, "windowTransportBytesReceived"),
				  TelemetryValue(snapshot, "windowTransportPacketsSent"),
				  TelemetryValue(snapshot, "windowTransportPacketsReceived")),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("transportTrafficAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("transportTrafficReason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Transport DTLS / connectivity / role"),
		 TelemetryDisplayList(snapshot, "transportDtlsStates") + QStringLiteral(" / ") +
			 TelemetryDisplayList(snapshot, "transportConnectivityStates") +
			 QStringLiteral(" / ") + TelemetryDisplayList(snapshot, "transportRoles"),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("transportStateAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("transportStateReason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Local device continuity"),
		 QCoreApplication::translate("MeetingUI", "%1 active / %2 expected; %3 stops, %4 ms")
			 .arg(TelemetryValue(snapshot, "activeLocalDeviceStreams"),
				  TelemetryValue(snapshot, "expectedLocalDeviceStreams"),
				  TelemetryValue(snapshot, "localDeviceUnexpectedStops"),
				  TelemetryValue(snapshot, "localDeviceInterruptionDurationMs")),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("localDeviceContinuityAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("localDeviceContinuityReason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Device format / video clock reset"),
		 QStringLiteral("%1 / %2").arg(
			 TelemetryValue(snapshot, "localDeviceFormatChanges"),
			 TelemetryValue(snapshot, "localDeviceClockResets")),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("localDeviceContinuityAvailability")).toString()),
		 snapshot.value(QStringLiteral("localDeviceContinuityAlgorithm")).toString()},
		{QCoreApplication::translate("MeetingUI", "Native device open / OS hotplug"),
		 QStringLiteral("%1 / %2").arg(
			 LocalizeTelemetryDisplayText(snapshot.value(
				 QStringLiteral("deviceOpenAvailability")).toString()),
			 LocalizeTelemetryDisplayText(snapshot.value(
				 QStringLiteral("deviceHotplugAvailability")).toString())),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("deviceStateAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("deviceOpenReason")).toString()) + QStringLiteral(" / ") +
			 LocalizeTelemetryDisplayText(snapshot.value(
				 QStringLiteral("deviceHotplugReason")).toString())},
		{QCoreApplication::translate("MeetingUI", "Requested / effective local media"),
		 QCoreApplication::translate("MeetingUI", "microphone %1/%2; camera %3/%4")
			 .arg(TelemetryValue(snapshot, "microphoneRequested"),
				  TelemetryValue(snapshot, "microphoneEffective"),
				  TelemetryValue(snapshot, "cameraRequested"),
				  TelemetryValue(snapshot, "cameraEffective")),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("deviceStateAvailability")).toString()),
		 QCoreApplication::translate("MeetingUI", "%1x%2 / %3 Hz / %4 channels")
			 .arg(TelemetryValue(snapshot, "actualCaptureWidth"),
				  TelemetryValue(snapshot, "actualCaptureHeight"),
				  TelemetryValue(snapshot, "actualCaptureSampleRate"),
				  TelemetryValue(snapshot, "actualCaptureChannels"))},
		{QCoreApplication::translate("MeetingUI", "Camera pipeline frames"),
		 QCoreApplication::translate("MeetingUI", "source %1; RTC input %2 / output %3 / dropped %4")
			 .arg(snapshot.value(QStringLiteral("cameraSourceAvailability")).toString() == QStringLiteral("VALID")
				  ? TelemetryValue(snapshot, "cameraSourceFrames") : QStringLiteral("--"),
				  snapshot.value(QStringLiteral("cameraRtcAvailability")).toString() == QStringLiteral("VALID")
				  ? TelemetryValue(snapshot, "cameraRtcInputFrames") : QStringLiteral("--"),
				  snapshot.value(QStringLiteral("cameraRtcAvailability")).toString() == QStringLiteral("VALID")
				  ? TelemetryValue(snapshot, "cameraRtcOutputFrames") : QStringLiteral("--"),
				  snapshot.value(QStringLiteral("cameraRtcAvailability")).toString() == QStringLiteral("VALID")
				  ? TelemetryValue(snapshot, "cameraRtcDroppedFrames") : QStringLiteral("--")),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("cameraSourceAvailability")).toString()) +
			 QStringLiteral(" / ") + LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("cameraRtcAvailability")).toString()),
		 QCoreApplication::translate("MeetingUI", "room %1 / publication %2; active %3")
			 .arg(TelemetryValue(snapshot, "cameraRoomGeneration"),
				  TelemetryValue(snapshot, "cameraPublicationEpoch"),
				  TelemetryValue(snapshot, "cameraPipelineActive"))},
		{QCoreApplication::translate("MeetingUI", "Device switch outcomes"),
		 QCoreApplication::translate("MeetingUI", "%1 attempts / %2 success / %3 failure / %4 timeout")
			 .arg(TelemetryValue(snapshot, "deviceSwitchAttempts"),
				  TelemetryValue(snapshot, "deviceSwitchSuccesses"),
				  TelemetryValue(snapshot, "deviceSwitchFailures"),
				  TelemetryValue(snapshot, "deviceSwitchTimeouts")),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("deviceFailureAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(
			 QStringLiteral("deviceFailureReason")).toString())},
	});
	tabs->widget(2)->setObjectName(QStringLiteral("telemetryNetworkDevices"));

	auto *operations = MakeTelemetryTable(tabs, {
		QCoreApplication::translate("MeetingUI", "Operation"),
		QCoreApplication::translate("MeetingUI", "Started / terminal"),
		QCoreApplication::translate("MeetingUI", "Success / failure / timeout / cancelled"),
		QCoreApplication::translate("MeetingUI", "Last duration")});
	for (const auto &value : snapshot.value(QStringLiteral("operationSummaries")).toList()) {
		const auto item = value.toMap();
		AddTelemetryRow(operations, {
			LocalizeTelemetryDisplayText(item.value(QStringLiteral("kind")).toString()),
			QStringLiteral("%1 / %2").arg(item.value(QStringLiteral("started")).toString(),
				item.value(QStringLiteral("terminal")).toString()),
			QStringLiteral("%1 / %2 / %3 / %4").arg(
				item.value(QStringLiteral("success")).toString(),
				item.value(QStringLiteral("failure")).toString(),
				item.value(QStringLiteral("timeout")).toString(),
				item.value(QStringLiteral("cancelled")).toString()),
			TelemetryValue(item, "lastDurationMs", "ms")});
	}
	operations->setObjectName(QStringLiteral("telemetryOperations"));
	tabs->addTab(operations, QCoreApplication::translate("MeetingUI", "Operations"));

	auto *productChains = MakeTelemetryTable(tabs, {
		QCoreApplication::translate("MeetingUI", "Metric ID"),
		QCoreApplication::translate("MeetingUI", "Product-chain status"),
		QCoreApplication::translate("MeetingUI", "Reason / boundary")});
	for (const auto &value : snapshot.value(QStringLiteral("metricProductChains")).toList()) {
		const auto item = value.toMap();
		AddTelemetryRow(productChains, {
			item.value(QStringLiteral("metricId")).toString(),
			LocalizeTelemetryDisplayText(item.value(QStringLiteral("status")).toString()),
			LocalizeTelemetryDisplayText(item.value(QStringLiteral("reason")).toString())});
	}
	productChains->setObjectName(QStringLiteral("telemetryProductChains"));
	tabs->addTab(productChains,
		QCoreApplication::translate("MeetingUI", "Capability boundaries"));

	auto *resourcePage = new QWidget(tabs);
	auto *resourceLayout = new QVBoxLayout(resourcePage);
	const auto store = livekit::telemetry::InstalledTelemetryHistoryStore();
	auto *trend = new TelemetryTrendWidget(
		store ? store->CurrentRecords()
		      : std::vector<livekit::telemetry::SafeTelemetryRecordPtr>{}, resourcePage);
	trend->setObjectName(QStringLiteral("telemetryTrend"));
	resourceLayout->addWidget(trend);
	auto *resources = MakeTelemetryTable(resourcePage, {
		QCoreApplication::translate("MeetingUI", "Metric"),
		QCoreApplication::translate("MeetingUI", "Current"),
		QCoreApplication::translate("MeetingUI", "Window"),
		QCoreApplication::translate("MeetingUI", "Availability")});
	AddTelemetryRow(resources, {QCoreApplication::translate("MeetingUI", "Process CPU"),
		TelemetryValue(snapshot, "processCpuPercent", "%"), QString(),
		LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("cpuAvailability")).toString())});
	AddTelemetryRow(resources, {QCoreApplication::translate("MeetingUI", "Private memory"),
		snapshot.value(QStringLiteral("memoryAvailability")).toString() == QStringLiteral("VALID")
			? QString::number(snapshot.value(QStringLiteral("privateBytes")).toULongLong() / 1048576.0, 'f', 1) + QStringLiteral(" MiB")
			: QStringLiteral("--"),
		QStringLiteral("%1 - %2 MiB").arg(
			snapshot.value(QStringLiteral("minimumPrivateBytes")).toULongLong() / 1048576.0, 0, 'f', 1).arg(
			snapshot.value(QStringLiteral("maximumPrivateBytes")).toULongLong() / 1048576.0, 0, 'f', 1),
		LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("memoryAvailability")).toString())});
	AddTelemetryRow(resources, {QCoreApplication::translate("MeetingUI", "Threads / handles"),
		QStringLiteral("%1 / %2").arg(
			snapshot.value(QStringLiteral("processThreadCount")).toString(),
			snapshot.value(QStringLiteral("processHandleCount")).toString()),
		QCoreApplication::translate("MeetingUI", "samples=%1, coverage=%2%").arg(
			snapshot.value(QStringLiteral("resourceTrendSamples")).toString(),
			QString::number(snapshot.value(QStringLiteral("resourceTrendCoverage")).toDouble() * 100.0, 'f', 1)),
		LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("resourceTrendAvailability")).toString())});
	AddTelemetryRow(resources, {QCoreApplication::translate("MeetingUI", "Internal bindings / local streams / router slots"),
		QStringLiteral("%1 / %2 / %3").arg(
			TelemetryValue(snapshot, "activeNativeBindings"),
			TelemetryValue(snapshot, "activeLocalMediaStreams"),
			TelemetryValue(snapshot, "activeRouterSlots")),
		QString(), LocalizeTelemetryDisplayText(snapshot.value(
			QStringLiteral("internalResourceAvailability")).toString())});
	AddTelemetryRow(resources, {QCoreApplication::translate("MeetingUI", "Router submitted / replaced / capacity drops"),
		QStringLiteral("%1 / %2 / %3").arg(
			TelemetryValue(snapshot, "routerFramesSubmitted"),
			TelemetryValue(snapshot, "routerFramesReplaced"),
			TelemetryValue(snapshot, "routerCapacityDrops")),
		QString(), LocalizeTelemetryDisplayText(snapshot.value(
			QStringLiteral("routerQueueAvailability")).toString())});
	AddTelemetryRow(resources, {QCoreApplication::translate("MeetingUI", "Export queue / post-stop return"),
		LocalizeTelemetryDisplayText(snapshot.value(
			QStringLiteral("exportQueueAvailability")).toString()) + QStringLiteral(" / ") +
			LocalizeTelemetryDisplayText(snapshot.value(
				QStringLiteral("resourceReturnAvailability")).toString()),
		QString(), LocalizeTelemetryDisplayText(snapshot.value(
			QStringLiteral("resourceReturnReason")).toString())});
	AddTelemetryRow(resources, {QCoreApplication::translate("MeetingUI", "Typed anomaly density"),
		QCoreApplication::translate("MeetingUI", "%1 events / %2 per hour")
			.arg(TelemetryValue(snapshot, "stabilityAnomalies"),
				 TelemetryValue(snapshot, "stabilityAnomaliesPerHour")),
		QCoreApplication::translate("MeetingUI", "operation %1 / sampler %2 / device %3 / media %4")
			.arg(TelemetryValue(snapshot, "stabilityOperationFailures"),
				 TelemetryValue(snapshot, "stabilitySamplerInterruptions"),
				 TelemetryValue(snapshot, "stabilityDeviceStops"),
				 TelemetryValue(snapshot, "stabilityMediaFailures")),
		LocalizeTelemetryDisplayText(snapshot.value(
			QStringLiteral("stabilityAnomalyDensityAvailability")).toString())});
	resourceLayout->addWidget(resources, 1);
	resources->setObjectName(QStringLiteral("telemetryResources"));
	tabs->addTab(resourcePage, QCoreApplication::translate("MeetingUI", "Resources"));

	auto *timeline = MakeTelemetryTable(tabs, {
		QCoreApplication::translate("MeetingUI", "Time"),
		QCoreApplication::translate("MeetingUI", "Revision"),
		QCoreApplication::translate("MeetingUI", "Status"),
		QCoreApplication::translate("MeetingUI", "Video / audio / render")});
	if (store) {
		for (const auto &record : store->CurrentRecords()) {
			if (!record) continue;
			const auto &s = record->snapshot;
			AddTelemetryRow(timeline, {
				QDateTime::fromMSecsSinceEpoch(record->captured_utc_ms).toString(QStringLiteral("HH:mm:ss")),
				QString::number(s.revision),
				LocalizeTelemetryDisplayText(QString::fromLatin1(
					livekit::telemetry::AvailabilityName(s.availability))),
				QStringLiteral("%1 / %2 / %3").arg(
					LocalizeTelemetryDisplayText(QString::fromLatin1(livekit::telemetry::AvailabilityName(s.remote_video_first_frame_availability))),
					LocalizeTelemetryDisplayText(QString::fromLatin1(livekit::telemetry::AvailabilityName(s.audio_quality_availability))),
					LocalizeTelemetryDisplayText(QString::fromLatin1(livekit::telemetry::AvailabilityName(s.render_stall_availability))))});
		}
	}
	timeline->setObjectName(QStringLiteral("telemetryTimeline"));
	tabs->addTab(timeline, QCoreApplication::translate("MeetingUI", "Timeline"));

	auto *all = MakeTelemetryTable(tabs, {
		QCoreApplication::translate("MeetingUI", "Field"),
		QCoreApplication::translate("MeetingUI", "Value")});
	for (auto it = snapshot.cbegin(); it != snapshot.cend(); ++it) {
		if (it.value().type() == QVariant::List || it.value().type() == QVariant::Map) continue;
		if (!IsSafeTelemetryDisplayEntry(it.key(), it.value())) continue;
		AddTelemetryRow(all, {
			LocalizeTelemetryFieldName(it.key()),
			TelemetryDisplayVariant(it.value())});
		auto *field = all->item(all->rowCount() - 1, 0);
		field->setData(Qt::UserRole, it.key());
		field->setToolTip(QCoreApplication::translate(
			"MeetingUI", "Internal field: %1").arg(it.key()));
	}
	all->setObjectName(QStringLiteral("telemetryAllMetrics"));
	tabs->addTab(all, QCoreApplication::translate("MeetingUI", "All metrics"));

	auto *historyPage = new QWidget(tabs);
	auto *historyLayout = new QVBoxLayout(historyPage);
	const auto status = store ? store->Status() : nullptr;
	auto *historyEnabled = new QCheckBox(
		QCoreApplication::translate("MeetingUI", "Keep local telemetry history"), historyPage);
	historyEnabled->setObjectName(QStringLiteral("telemetryHistoryEnabled"));
	historyEnabled->setChecked(status && status->history_enabled);
	historyEnabled->setEnabled(store != nullptr);
	historyLayout->addWidget(historyEnabled);
	auto *historyStatus = new QLabel(status
		? QCoreApplication::translate(
			"MeetingUI", "%1 / %2 / MET-06 drops=%3, write failures=%4").arg(
			LocalizeTelemetryDisplayText(QString::fromLatin1(
				livekit::telemetry::AvailabilityName(status->availability))),
			LocalizeTelemetryDisplayText(QString::fromStdString(status->reason)),
			QString::number(status->queue_drops),
			QString::number(status->write_failures))
		: QStringLiteral("%1 / %2").arg(
			LocalizeTelemetryDisplayText(QStringLiteral("UNSUPPORTED")),
			LocalizeTelemetryDisplayText(QStringLiteral("history_store_not_installed"))), historyPage);
	historyStatus->setWordWrap(true);
	historyLayout->addWidget(historyStatus);
	auto *reports = new QListWidget(historyPage);
	reports->setObjectName(QStringLiteral("telemetryReports"));
	if (status) {
		for (const auto &entry : status->reports) {
			auto *item = new QListWidgetItem(
				QCoreApplication::translate("MeetingUI", "%1  %2 KiB  records=%3").arg(
					QDateTime::fromMSecsSinceEpoch(entry.created_utc_ms).toString(Qt::ISODate),
					QString::number(entry.size_bytes / 1024),
					QString::number(entry.record_count)), reports);
			item->setData(Qt::UserRole, QString::fromStdString(entry.record_id));
			item->setData(Qt::UserRole + 1, QVariant::fromValue<qlonglong>(entry.created_utc_ms));
			item->setData(Qt::UserRole + 2, static_cast<qulonglong>(entry.size_bytes));
		}
	}
	historyLayout->addWidget(reports, 1);
	auto *rangeRow = new QHBoxLayout();
	auto *limitRange = new QCheckBox(
		QCoreApplication::translate("MeetingUI", "Time range"), historyPage);
	limitRange->setObjectName(QStringLiteral("telemetryRangeEnabled"));
	auto *rangeStart = new QDateTimeEdit(historyPage);
	auto *rangeEnd = new QDateTimeEdit(historyPage);
	rangeStart->setObjectName(QStringLiteral("telemetryRangeStart"));
	rangeEnd->setObjectName(QStringLiteral("telemetryRangeEnd"));
	for (auto *edit : {rangeStart, rangeEnd}) {
		edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
		edit->setTimeSpec(Qt::UTC);
		edit->setCalendarPopup(true);
		edit->setEnabled(false);
	}
	rangeRow->addWidget(limitRange);
	rangeRow->addWidget(new QLabel(
		QCoreApplication::translate("MeetingUI", "UTC from"), historyPage));
	rangeRow->addWidget(rangeStart);
	rangeRow->addWidget(new QLabel(
		QCoreApplication::translate("MeetingUI", "to"), historyPage));
	rangeRow->addWidget(rangeEnd);
	historyLayout->addLayout(rangeRow);
	auto *selectionStatus = new QLabel(historyPage);
	selectionStatus->setWordWrap(true);
	historyLayout->addWidget(selectionStatus);
	QObject::connect(limitRange, &QCheckBox::toggled, dialog,
		[rangeStart, rangeEnd](bool enabled) {
			rangeStart->setEnabled(enabled);
			rangeEnd->setEnabled(enabled);
		});
	auto *historyButtons = new QHBoxLayout();
	auto *exportButton = new QPushButton(
		dialog->style()->standardIcon(QStyle::SP_DialogSaveButton),
		QCoreApplication::translate("MeetingUI", "Export"), historyPage);
	exportButton->setObjectName(QStringLiteral("telemetryExport"));
	exportButton->setEnabled(reports->currentItem() != nullptr);
	auto *clearButton = new QPushButton(
		dialog->style()->standardIcon(QStyle::SP_TrashIcon),
		QCoreApplication::translate("MeetingUI", "Clear selected"), historyPage);
	clearButton->setObjectName(QStringLiteral("telemetryClearSelected"));
	clearButton->setEnabled(reports->currentItem() != nullptr);
	historyButtons->addWidget(exportButton);
	historyButtons->addWidget(clearButton);
	historyButtons->addStretch();
	historyLayout->addLayout(historyButtons);
	tabs->addTab(historyPage, QCoreApplication::translate("MeetingUI", "Reports"));

	QObject::connect(reports, &QListWidget::currentItemChanged, dialog,
		[clearButton, exportButton, rangeStart, rangeEnd, selectionStatus]
			(QListWidgetItem *current) {
			clearButton->setEnabled(current != nullptr);
			exportButton->setEnabled(current != nullptr);
			if (!current) {
				selectionStatus->clear();
				return;
			}
			const auto end = QDateTime::fromMSecsSinceEpoch(
				current->data(Qt::UserRole + 1).toLongLong(), Qt::UTC);
			rangeEnd->setDateTime(end);
			rangeStart->setDateTime(end.addSecs(-3600));
			const auto legacy = current->data(Qt::UserRole).toString()
				.startsWith(QStringLiteral("cohavora-telemetry-v1-"));
			selectionStatus->setText(QCoreApplication::translate(
				"MeetingUI", "Estimated %1 KiB; %2").arg(
				QString::number(current->data(Qt::UserRole + 2)
					.toULongLong() / 1024),
				legacy ? QCoreApplication::translate("MeetingUI",
					"Legacy report: run identity unavailable")
					: QCoreApplication::translate("MeetingUI",
					"Includes metrics, events and available stability evidence")));
		});
	QObject::connect(historyEnabled, &QCheckBox::toggled, dialog, [store](bool enabled) {
		if (store) store->SetHistoryEnabled(enabled);
		QSettings().setValue(QStringLiteral("telemetry/historyEnabled"), enabled);
	});
	QObject::connect(exportButton, &QPushButton::clicked, dialog,
		[dialog, reports, limitRange, rangeStart, rangeEnd] {
			const auto *item = reports->currentItem();
			if (!item) return;
			const auto first = limitRange->isChecked()
				? rangeStart->dateTime().toMSecsSinceEpoch() : 0;
			const auto last = limitRange->isChecked()
				? rangeEnd->dateTime().toMSecsSinceEpoch() : 0;
			if (first > last) {
				QMessageBox::warning(dialog,
					QCoreApplication::translate("MeetingUI", "Telemetry export"),
					QCoreApplication::translate("MeetingUI", "Invalid time range"));
				return;
			}
			ShowTelemetryExport(dialog,
				item->data(Qt::UserRole).toString().toStdString(),
				first, last);
		});
	QObject::connect(clearButton, &QPushButton::clicked, dialog,
		[store, reports, clearButton] {
			const auto *item = reports->currentItem();
			if (!store || !item) return;
			const auto id = item->data(Qt::UserRole).toString().toStdString();
			clearButton->setEnabled(false);
			const QPointer<QListWidget> listGuard(reports);
			const auto dispatcher = OpenMeeting::detail::QtDispatchEndpoint::Create();
			store->ClearReport(id, [dispatcher, listGuard, id](bool removed, std::string) {
				if (dispatcher) dispatcher->Post( [listGuard, id, removed] {
					if (!listGuard || !removed) return;
					for (auto row = 0; row != listGuard->count(); ++row) {
						if (listGuard->item(row)->data(Qt::UserRole).toString().toStdString() == id) {
							delete listGuard->takeItem(row);
							break;
						}
					}
				});
			});
		});

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
	buttons->setObjectName(QStringLiteral("telemetryDetailsButtons"));
	buttons->button(QDialogButtonBox::Close)->setIcon(
		dialog->style()->standardIcon(QStyle::SP_DialogCloseButton));
	QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
	layout->addWidget(buttons);
	AppTheme::makeDialogAdaptive(*dialog, QSize(900, 640));
	dialog->show();
	dialog->raise();
	dialog->activateWindow();
	return dialog;
}

} // namespace MeetingUI
