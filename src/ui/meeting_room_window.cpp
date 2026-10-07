#include "src/ui/meeting_encryption_panel.h"
#include "telemetry_live_dialog.h"
#include "src/core/session_shutdown_service.h"
#include <QtCore/QCoreApplication>
#include "src/ui/meeting_room_window.h"
#include "src/ui/remote_control_ui.h"
#include "src/ui/whiteboard/accessible_combo_box.h"
#include "src/ui/app_theme.h"
#include "src/ui/whiteboard/annotation_overlay_window.h"
#include "src/ui/whiteboard/whiteboard_panel.h"
#include "src/ui/meeting_log_console.h"
#include "src/ui/telemetry_dialogs.h"
#include "src/telemetry/telemetry_report.h"
#include "src/telemetry/diagnostic_pipeline.h"
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDateTimeEdit>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QApplication>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QScrollArea>
#include "src/ui/native_child_geometry.h"
#include <QtWidgets/QStyle>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableWidget>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QIcon>
#include <QtGui/QPainterPath>
#include <QtGui/QFont>
#include <QtGui/QClipboard>
#include <QtGui/QWindow>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QSignalBlocker>
#include <QtCore/QThread>
#include <algorithm>
#include <cmath>
#include <exception>
#include <set>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#endif

namespace MeetingUI {
namespace {

// The popup owns only a cancellable subscription, never a native driver job.
// Even cached results are queued so no action is populated before popup().
template <typename Discovery, typename Apply>
void loadDeviceMenu(QMenu *menu, Discovery &discovery, Apply apply) {
    auto gate = OpenMeeting::QtCallbackGate<QMenu>::Create(menu);
    auto request = std::make_shared<typename Discovery::Request>();
    QObject::connect(menu, &QMenu::aboutToHide, menu, [gate, request] {
        gate->Revoke();
        request->reset();
    });
    QTimer::singleShot(0, menu, [&discovery, gate, request, apply = std::move(apply)] {
        if (!gate->active()) return;
        *request = discovery.request([gate, apply](const auto &result) {
            gate->Post([apply, result](QMenu *target) { apply(*target, result); });
        });
    });
}

QSize bannerSize(QLabel &label, int availableWidth, int preferredWidth) {
 label.setWordWrap(true);
 label.ensurePolished();
 const int width = std::max(1, std::min(availableWidth - 32,
     std::max(preferredWidth, label.sizeHint().width())));
 return QSize(width, std::max(32, label.heightForWidth(width)));
}

constexpr int kBottomBarHeight = 72;
constexpr int kBottomBarPadding = 12;
constexpr int kBottomBarGap = 6;
constexpr int kBottomBarEndWidth = 88;
constexpr int kBottomBarPreferredToolWidth = 76;
#if defined(Q_OS_WIN)
constexpr int kNativeVerticalFramePixels = 1;
#endif

void ShowMeetingWarning(QWidget* parent, const char* id, const char* dismissId,
                           const QString& title, const QString& text) {
	auto* message = new QMessageBox(QMessageBox::Warning, title, text,
		QMessageBox::Ok, parent);
	message->setObjectName(QString::fromLatin1(id));
	message->setAttribute(Qt::WA_DeleteOnClose);
	AppTheme::setTone(*message, AppTheme::Tone::Dark);
	message->button(QMessageBox::Ok)->setObjectName(QString::fromLatin1(dismissId));
	message->open();
}

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
	}

protected:
	void paintEvent(QPaintEvent *) override {
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		const auto plot = rect().adjusted(44, 18, -18, -30);
		painter.setPen(QColor(0x68, 0x70, 0x7a));
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
		painter.setPen(QPen(QColor(0x2f, 0xc4, 0x77), 2));
		painter.drawPolyline(cpu);
		painter.setPen(QPen(QColor(0x42, 0x9b, 0xe8), 2));
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

bool SameViewportIntentContent(const livekit::ViewportIntent &left,
		const livekit::ViewportIntent &right) {
	const auto sameKey = [](const auto &a, const auto &b) {
		return a.has_value() == b.has_value() && (!a || *a == *b);
	};
	return left.mode == right.mode && left.page == right.page &&
		left.page_size == right.page_size &&
		sameKey(left.page_anchor, right.page_anchor) &&
		sameKey(left.pinned, right.pinned) &&
		sameKey(left.selected_share, right.selected_share) &&
		left.stage_content == right.stage_content &&
		left.stage_rect.x == right.stage_rect.x &&
		left.stage_rect.y == right.stage_rect.y &&
		left.stage_rect.width == right.stage_rect.width &&
		left.stage_rect.height == right.stage_rect.height &&
		left.device_pixel_ratio == right.device_pixel_ratio &&
		left.window_visible == right.window_visible &&
		left.minimized == right.minimized;
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
	QFileDialog dialog(parent, QCoreApplication::translate("MeetingUI", "Export telemetry report"));
	dialog.setObjectName(QStringLiteral("telemetryExportDirectory"));
	dialog.setOption(QFileDialog::DontUseNativeDialog);
	dialog.setOption(QFileDialog::ShowDirsOnly);
	dialog.setFileMode(QFileDialog::Directory);
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
	const auto destination = std::filesystem::path(directory.toStdWString());
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
					message.setInformativeText(QString::fromStdWString(
						result.report_directory.wstring()));
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
		 TelemetryValue(snapshot, "lastSubscribeToFirstDecodedMs", "ms"),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("firstVideoAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("firstVideoMeasurementPoint")).toString())},
		{QCoreApplication::translate("MeetingUI", "First visible render"),
		 QCoreApplication::translate("MeetingUI", "admission %1 / subscription %2")
			 .arg(TelemetryValue(snapshot, "lastAdmissionToFirstRenderMs", "ms"),
				  TelemetryValue(snapshot, "lastSubscribeToFirstRenderMs", "ms")),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("renderFirstFrameAvailability")).toString()),
		 LocalizeTelemetryDisplayText(snapshot.value(QStringLiteral("renderMeasurementPoint")).toString())},
		{QCoreApplication::translate("MeetingUI", "First remote PCM"),
		 TelemetryValue(snapshot, "lastSubscribeToFirstPcmMs", "ms"),
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
			item->setData(Qt::UserRole + 1, entry.created_utc_ms);
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

namespace {

bool isChatSenderPlaceholderName(const QString &name) {
	const auto normalized = name.trimmed().toCaseFolded();
	return normalized.isEmpty()
		|| normalized.startsWith(QStringLiteral("pa_"))
		|| normalized == QStringLiteral("participant-name")
		|| normalized == QStringLiteral("participant_name")
		|| normalized == QStringLiteral("participant name");
}

QString resolveParticipantDisplayName(
	const std::shared_ptr<OpenMeeting::MeetingCoordinator> &coordinator,
	const QString &senderIdentity,
	const QString &senderName) {
	QString displayName = senderName.trimmed();
	const auto needsParticipantLookup = isChatSenderPlaceholderName(displayName);
	if (needsParticipantLookup) {
		displayName = senderIdentity;
	}
	if (coordinator && (needsParticipantLookup || displayName == senderIdentity)) {
		for (const auto &participant : coordinator->participants()) {
			if (participant.identity != senderIdentity && participant.identity != senderName) {
				continue;
			}
			const auto participantName = participant.name.trimmed();
			if (!isChatSenderPlaceholderName(participantName)) {
				displayName = participantName;
			}
			break;
		}
	}
	return displayName.isEmpty() ? QCoreApplication::translate("MeetingUI", "Participants") : displayName;
}

} // namespace

// ----------------------------------------------------
// VideoTileWidget 实现
// ----------------------------------------------------

VideoTileWidget::VideoTileWidget(const QString &displayName, bool isLocal, QWidget *parent, bool isScreenShare)
	: Ui::RpWidget(parent)
	, _displayName(displayName)
	, _isLocal(isLocal) {
	_isScreenShare = isScreenShare;
	setMouseTracking(true);
	setAttribute(Qt::WA_OpaquePaintEvent, false);

	_visualizer = new AudioVisualizerWidget(this, 7);
	_visualizer->setBarColor(QColor(0, 180, 42));
	_visualizer->hide();

	setupVolumeControls();
}

void VideoTileWidget::setupVolumeControls() {
	_pinBtn = new QPushButton(QString::fromUtf8("📌"), this);
	_pinBtn->setFixedSize(28, 28);
	_pinBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Pin / Unpin Video"));
	MeetingUI::AppTheme::setStyleVariant(*_pinBtn, "meeting-room-window-pinbtn");
	_pinBtn->hide();

	connect(_pinBtn, &QPushButton::clicked, [this] {
		emit pinToggled(!_isPinned); // The window owns the single selected identity.
	});

	if (_isLocal || _isScreenShare) return;

	_volBtn = new QPushButton(QString::fromUtf8("🔊"), this);
	_volBtn->setFixedSize(28, 28);
	_volBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Adjust this participant's volume"));
	MeetingUI::AppTheme::setStyleVariant(*_volBtn, "meeting-room-window-volbtn");
	_volBtn->hide();

	_volPopup = new QWidget(this);
	auto *volumeLayout = new QHBoxLayout(_volPopup);
 volumeLayout->setContentsMargins(8, 8, 8, 8);
 volumeLayout->setSpacing(6);
	MeetingUI::AppTheme::setStyleVariant(*_volPopup, "meeting-room-window-volpopup");
	_volPopup->hide();

	_muteRemoteBtn = new QPushButton(QString::fromUtf8("🔊"), _volPopup);
	_muteRemoteBtn->setFixedSize(26, 26);
	volumeLayout->addWidget(_muteRemoteBtn);
	MeetingUI::AppTheme::setStyleVariant(*_muteRemoteBtn, "meeting-room-window-muteremotebtn");

	_volSlider = new QSlider(Qt::Horizontal, _volPopup);
	_volSlider->setRange(0, 200);
	_volSlider->setValue(100);
	_volSlider->setMinimumWidth(100);
	volumeLayout->addWidget(_volSlider, 1);
	MeetingUI::AppTheme::setStyleVariant(*_volSlider, "meeting-room-window-volslider");

	_volLabel = new QLabel(QString::fromUtf8("100%"), _volPopup);
	volumeLayout->addWidget(_volLabel);
	_volPopup->adjustSize();
	MeetingUI::AppTheme::setStyleVariant(*_volLabel, "meeting-room-window-vollabel");

	connect(_volBtn, &QPushButton::clicked, [this] {
		if (_volPopup->isVisible()) {
			_volPopup->hide();
		} else {
			_volPopup->show();
			_volPopup->raise();
		}
	});

	connect(_muteRemoteBtn, &QPushButton::clicked, [this] {
		_isLocallyMuted = !_isLocallyMuted;
		_muteRemoteBtn->setText(_isLocallyMuted ? QString::fromUtf8("🔇") : QString::fromUtf8("🔊"));
		MeetingUI::AppTheme::setStyleVariant(*_muteRemoteBtn, _isLocallyMuted ? "meeting-room-window-muteremotebtn-2-active" : "meeting-room-window-muteremotebtn-2-normal");
		_volBtn->setText(_isLocallyMuted ? QString::fromUtf8("🔇") : QString::fromUtf8("🔊"));
		remoteLocalMuteToggled(_isLocallyMuted);
	});

	connect(_volSlider, &QSlider::valueChanged, [this](int value) {
		_remoteVolume = static_cast<float>(value) / 100.0f;
		_volLabel->setText(QString("%1%").arg(value));
		if (_isLocallyMuted && value > 0) {
			_isLocallyMuted = false;
			_muteRemoteBtn->setText(QString::fromUtf8("🔊"));
			MeetingUI::AppTheme::setStyleVariant(*_muteRemoteBtn, "meeting-room-window-muteremotebtn-3");
			_volBtn->setText(QString::fromUtf8("🔊"));
			remoteLocalMuteToggled(false);
		}
		remoteVolumeChanged(_remoteVolume);
	});
}

void VideoTileWidget::enterEventHook(QEnterEvent *e) {
	if (_pinBtn && !_remoteInputEnabled) _pinBtn->show();
	if (_volBtn) _volBtn->show();
	Ui::RpWidget::enterEventHook(e);
}

void VideoTileWidget::leaveEventHook(QEvent *e) {
	if (_pinBtn && !_isPinned) _pinBtn->hide();
	if (_volBtn && (!_volPopup || !_volPopup->isVisible())) {
		_volBtn->hide();
	}
	Ui::RpWidget::leaveEventHook(e);
}

void VideoTileWidget::resizeEvent(QResizeEvent *e) {
	Ui::RpWidget::resizeEvent(e);
	const int w = width();
	const int h = height();

	if (_pinBtn) {
		_pinBtn->setGeometry(pinButtonRect());
	}
	if (_volBtn) {
		_volBtn->move(w - 36, 10);
	}
	if (_volPopup) {
		_volPopup->move(std::max(0, w - _volPopup->width() - 10), 42);
	}
	if (_visualizer) {
		_visualizer->setGeometry((w - 90) / 2, h - 36, 90, 24);
		_visualizer->raise();
	}
}

void VideoTileWidget::setDisplayName(const QString &name) {
	if (_displayName == name) return;
	_displayName = name;
	invalidatePresentation();
}

void VideoTileWidget::setVideoActive(bool active) {
	if (_isVideoActive == active) return;
	_isVideoActive = active;
	livekit::render::VideoRenderFrame::Ptr retired;
	if (!active) {
		{
			std::lock_guard<std::mutex> lock(_frameMutex);
			_currentFrame = QImage();
			retired = std::exchange(_currentRenderFrame, {});
		}
		if (retired) {
			retired->SetRenderExpected(
				false, livekit::render::RenderExpectationReason::BindingEnded);
		}
	}
	updateRenderExpectation();
	invalidatePresentation();
}

void VideoTileWidget::setAudioMuted(bool muted) {
	if (_isAudioMuted == muted) return;
	_isAudioMuted = muted;
	if (muted) {
		_isSpeaking = false;
		_audioLevel = 0.0f;
		if (_visualizer) {
			_visualizer->setActive(false);
			_visualizer->hide();
		}
	}
	invalidatePresentation();
}

void VideoTileWidget::setConnectionQuality(livekit::ConnectionQuality quality) {
	if (_connectionQuality == quality) return;
	_connectionQuality = quality;
	invalidatePresentation();
}

void VideoTileWidget::setVideoStreamPaused(bool paused) {
	if (_isVideoStreamPaused == paused) return;
	_isVideoStreamPaused = paused;
	updateRenderExpectation();
	invalidatePresentation();
}

void VideoTileWidget::setVideoSubscriptionError(
		livekit::TrackPublication::SubscriptionError error) {
	if (_videoSubscriptionError == error) return;
	_videoSubscriptionError = error;
	if (error != livekit::TrackPublication::SubscriptionError::None) setFrame({});
	updateRenderExpectation();
	invalidatePresentation();
}

VideoTileWidget::~VideoTileWidget() {
	livekit::render::VideoRenderFrame::Ptr retired;
	{
		std::lock_guard<std::mutex> lock(_frameMutex);
		retired = std::exchange(_currentRenderFrame, {});
	}
	if (retired) {
		retired->SetRenderExpected(
			false, livekit::render::RenderExpectationReason::BindingEnded);
	}
}

void VideoTileWidget::setSpeaking(bool speaking, float level) {
	if (_isSpeaking == speaking && _audioLevel == level) return;
	_isSpeaking = speaking;
	_audioLevel = level;
	if (_visualizer) {
		_visualizer->setActive(speaking);
		if (speaking) {
			_visualizer->setAudioLevel(level);
			_visualizer->show();
			_visualizer->raise();
		} else {
			_visualizer->hide();
		}
	}
	invalidatePresentation();
}

void VideoTileWidget::invalidatePresentation() {
	_hardwareDecoration = {};
	update();
	emit presentationChanged();
}

void VideoTileWidget::setPinned(bool pinned) {
	if (_isPinned == pinned) return;
	_isPinned = pinned;
	MeetingUI::AppTheme::setStyleVariant(*_pinBtn, pinned ? "meeting-room-window-pinbtn-2-active" : "meeting-room-window-pinbtn-2-normal");
	_pinBtn->setVisible(!_remoteInputEnabled && (pinned || underMouse()));
	invalidatePresentation();
}

void VideoTileWidget::setPipMode(bool pip) {
	if (_isPip == pip) return;
	_isPip = pip;
	invalidatePresentation();
}

QRect VideoTileWidget::pinButtonRect() const {
	return QRect(_volBtn ? width() - 70 : width() - 36, 10, 28, 28);
}

QImage VideoTileWidget::hardwareDecoration(const QSize &pixels, bool hasFrame, bool hovered) {
	if (pixels.isEmpty() || size().isEmpty()) return {};
	if (!_hardwareDecoration.isNull() && _hardwareDecoration.size() == pixels &&
		_decorationLogicalSize == size() && _decorationHasFrame == hasFrame &&
		_decorationHovered == hovered) return _hardwareDecoration;
	_hardwareDecoration = QImage(pixels, QImage::Format_RGBA8888_Premultiplied);
	_hardwareDecoration.fill(Qt::transparent);
	_decorationLogicalSize = size();
	_decorationHasFrame = hasFrame;
	_decorationHovered = hovered;
	QPainter painter(&_hardwareDecoration);
	painter.scale(double(pixels.width()) / width(), double(pixels.height()) / height());
	paintCard(painter, true, hasFrame, hovered);
	painter.end();
	return _hardwareDecoration;
}

void VideoTileWidget::setHardwareCanvasMode(bool enabled) {
	if (_useHardwareCanvas == enabled) return;
	_useHardwareCanvas = enabled;
	updateRenderExpectation();
	update();
}

void VideoTileWidget::setFrame(
		const QImage &image,
		livekit::render::VideoRenderFrame::Ptr renderFrame) {
	livekit::render::VideoRenderFrame::Ptr retired;
	livekit::render::VideoRenderFrame::Ptr current;
	{
		std::lock_guard<std::mutex> lock(_frameMutex);
		_currentFrame = image;
		retired = std::exchange(_currentRenderFrame, std::move(renderFrame));
		current = _currentRenderFrame;
	}
	if (retired && retired != current &&
		(!current || !retired->renderMetadata().same_binding_as(
			current->renderMetadata()))) {
		retired->SetRenderExpected(
			false, livekit::render::RenderExpectationReason::BindingEnded);
	}
	updateRenderExpectation();
	update();
}

void VideoTileWidget::updateRenderExpectation() {
	livekit::render::VideoRenderFrame::Ptr frame;
	{
		std::lock_guard<std::mutex> lock(_frameMutex);
		frame = _currentRenderFrame;
	}
	if (!frame) return;
	const bool minimized = window() && window()->isMinimized();
	const bool expected = !_useHardwareCanvas && _isVideoActive &&
		_videoSubscriptionError == livekit::TrackPublication::SubscriptionError::None &&
		!_isVideoStreamPaused && isVisible() && !minimized &&
		width() > 0 && height() > 0;
	frame->SetRenderExpected(
		expected,
		expected ? livekit::render::RenderExpectationReason::SurfaceVisible
			: minimized ? livekit::render::RenderExpectationReason::WindowMinimized
				: _isVideoStreamPaused
					? livekit::render::RenderExpectationReason::StreamPaused
					: livekit::render::RenderExpectationReason::SurfaceHidden);
}

void VideoTileWidget::showEvent(QShowEvent *e) {
	Ui::RpWidget::showEvent(e);
	updateRenderExpectation();
}

void VideoTileWidget::hideEvent(QHideEvent *e) {
	Ui::RpWidget::hideEvent(e);
	updateRenderExpectation();
}

void VideoTileWidget::changeEvent(QEvent *e) {
	Ui::RpWidget::changeEvent(e);
	if (e->type() == QEvent::WindowStateChange ||
		e->type() == QEvent::ParentChange) {
		updateRenderExpectation();
	}
}

void VideoTileWidget::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	paintCard(p, false, false, underMouse());
}

void VideoTileWidget::paintCard(QPainter &p, bool decorationOnly, bool hasFrame, bool hovered) {
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	p.setRenderHint(QPainter::TextAntialiasing);

	const QRect r = rect();

	if (_isPip && !decorationOnly) {
		QPainterPath path;
		path.addRoundedRect(r.adjusted(2, 2, -2, -2), 10, 10);
		p.setClipPath(path);
		p.fillPath(path, QColor(0x1a, 0x1d, 0x24));
	}

	if (_videoSubscriptionError != livekit::TrackPublication::SubscriptionError::None) {
		drawVideoPlaceholder(p, r);
	} else if (_isVideoActive) {
		if (!decorationOnly) drawVideoFrame(p, r);
		else if (_isVideoStreamPaused || !hasFrame) drawVideoPlaceholder(p, r);
	} else {
		drawAvatarPlaceholder(p, r);
	}

	if (_remoteInputEnabled && !decorationOnly) return;
	drawBottomNameTag(p, r);
	drawNetworkQualityBadge(p, r);
	if (decorationOnly && (hovered || _isPinned)) {
		p.save();
		const auto button = pinButtonRect();
		p.setPen(Qt::NoPen);
		p.setBrush(_isPinned ? QColor("#1677ff") : QColor(0, 0, 0, 150));
		p.drawRoundedRect(button, 14, 14);
		p.setPen(Qt::white);
		p.setFont(QFont("Segoe UI Emoji", 10));
		p.drawText(button, Qt::AlignCenter, QString::fromUtf8("📌"));
		p.restore();
	}

	// 画中画模式下的基础边框
	if (_isPip) {
		p.setClipping(false);
		p.setPen(QPen(_isSpeaking ? QColor(0x00, 0xb4, 0x2a) : QColor(0x86, 0x90, 0x9c), _isSpeaking ? 3.0 : 2.0));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 10, 10);
	}

	// 说话中：绘制高质感双层绿色呼吸发光光圈 (Active Speaker Halo)
	if (_isSpeaking && !_isAudioMuted) {
		p.save();
		p.setClipping(false);
		p.setBrush(Qt::NoBrush);

		int alpha = static_cast<int>(60 + 150 * std::clamp(_audioLevel * 4.0f, 0.1f, 1.0f));
		QPen outerGlow(QColor(0, 180, 42, alpha / 3), 6.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
		p.setPen(outerGlow);
		p.drawRoundedRect(r.adjusted(3, 3, -3, -3), _isPip ? 10 : 8, _isPip ? 10 : 8);

		QPen innerFocus(QColor(0, 180, 42, alpha), 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
		p.setPen(innerFocus);
		p.drawRoundedRect(r.adjusted(1, 1, -1, -1), _isPip ? 10 : 8, _isPip ? 10 : 8);

		p.restore();
	}
}

static std::pair<QColor, QColor> GenerateAvatarGradient(const QString &str) {
	uint32_t hash = 5381;
	QByteArray ba = str.toUtf8();
	for (char c : ba) {
		hash = ((hash << 5) + hash) + static_cast<uint8_t>(c);
	}
	static const std::vector<std::pair<QColor, QColor>> gradients = {
		{ QColor(0x3a, 0x7b, 0xd5), QColor(0x3a, 0x60, 0x73) }, // Sea Blue
		{ QColor(0x6a, 0x11, 0xcb), QColor(0x25, 0x75, 0xfc) }, // Purple-Blue
		{ QColor(0x11, 0x99, 0x8e), QColor(0x38, 0xef, 0x7d) }, // Emerald Green
		{ QColor(0xf1, 0x27, 0x11), QColor(0xf5, 0xaf, 0x19) }, // Sunset Amber
		{ QColor(0x8e, 0x2d, 0xe2), QColor(0x4a, 0x00, 0xe0) }, // Royal Purple
		{ QColor(0x00, 0xb4, 0xd8), QColor(0x00, 0x77, 0xb6) }, // Ocean Cyan
		{ QColor(0xe0, 0x56, 0xfd), QColor(0x68, 0x6d, 0xe0) }, // Magenta
		{ QColor(0xeb, 0x3b, 0x5a), QColor(0xfa, 0x82, 0x31) }, // Coral
	};
	return gradients[hash % gradients.size()];
}

void VideoTileWidget::drawAvatarPlaceholder(QPainter &p, const QRect &r) {
	p.fillRect(r, QColor(0x18, 0x1a, 0x22));

	const int cx = r.center().x();
	const int cy = r.center().y();
	const int minDim = std::min(r.width(), r.height());
	const int outerRadius = std::clamp(minDim * 22 / 100, 28, 54);

	// 正在说话时：在头像外围绘制随音量动态扩散的声波涟漪光环
	if (_isSpeaking && !_isAudioMuted) {
		p.save();
		const int auraRadius = outerRadius + static_cast<int>(std::clamp(_audioLevel, 0.1f, 1.0f) * 14.0f);
		p.setPen(QPen(QColor(0x00, 0xb4, 0x2a, 100), 2.0));
		p.setBrush(QColor(0x00, 0xb4, 0x2a, 35));
		p.drawEllipse(QPoint(cx, cy - 14), auraRadius, auraRadius);
		p.restore();
	}

	// 质感色彩哈希渐变圆形头像
	p.save();
	auto [col1, col2] = GenerateAvatarGradient(_displayName.isEmpty() ? _identity : _displayName);
	QLinearGradient grad(cx - outerRadius, cy - 14 - outerRadius, cx + outerRadius, cy - 14 + outerRadius);
	grad.setColorAt(0.0, col1);
	grad.setColorAt(1.0, col2);

	p.setPen(Qt::NoPen);
	p.setBrush(grad);
	p.drawEllipse(QPoint(cx, cy - 14), outerRadius, outerRadius);

	QString initial = _displayName.isEmpty() ? (_identity.isEmpty() ? "U" : _identity.left(1)) : _displayName.left(1);
	if (!_displayName.isEmpty()) {
		QString clean = _displayName;
		clean.remove(QCoreApplication::translate("MeetingUI", " (Me)"));
		clean.remove(" (Host)");
		if (!clean.isEmpty()) {
			initial = clean.left(1).toUpper();
		}
	}
	QFont avatarFont("Microsoft YaHei", outerRadius * 8 / 10, QFont::Bold);
	p.setFont(avatarFont);
	p.setPen(Qt::white);
	QRect avatarRect(cx - outerRadius, cy - 14 - outerRadius, outerRadius * 2, outerRadius * 2);
	p.drawText(avatarRect, Qt::AlignCenter, initial);
	p.restore();

	// 昵称与麦克风指示
	QFont font("Microsoft YaHei", std::clamp(minDim * 6 / 100, 9, 12), QFont::Bold);
	p.setFont(font);
	QFontMetrics fm(font);
	const auto label = fm.elidedText(_displayName, Qt::ElideRight, std::max(0, r.width() - 40));
	const int textW = fm.horizontalAdvance(label);
	const int totalW = textW + 24;
	const int startX = cx - totalW / 2;
	const int nameY = cy + outerRadius + 6;

	const int micX = startX + 6;
	const int micY = nameY + 6;
	p.setPen(QPen(_isAudioMuted ? QColor(0xf5, 0x3f, 0x3f) : QColor(0x00, 0xb4, 0x2a), 1.5, Qt::SolidLine, Qt::RoundCap));
	p.drawRoundedRect(QRect(micX - 3, micY - 5, 6, 8), 3, 3);
	p.drawLine(micX, micY + 3, micX, micY + 6);
	p.drawLine(micX - 4, micY + 6, micX + 4, micY + 6);
	if (_isAudioMuted) {
		p.drawLine(micX - 5, micY - 6, micX + 5, micY + 7);
	}

	p.setPen(QColor(0xf0, 0xf2, 0xf5));
	p.drawText(QRect(startX + 18, nameY - 2, textW + 10, 20), Qt::AlignLeft | Qt::AlignVCenter, label);
}

void VideoTileWidget::drawNetworkQualityBadge(QPainter &p, const QRect &r) {
	p.save();
	const int bx = r.x() + 10;
	const int by = r.y() + 12;
	int bars = 0;
	QColor color(0x86, 0x90, 0x9c);
	switch (_connectionQuality) {
	case livekit::ConnectionQuality::Excellent:
		bars = 3;
		color = QColor(0x00, 0xb4, 0x2a);
		break;
	case livekit::ConnectionQuality::Good:
		bars = 2;
		color = QColor(0x52, 0xc4, 0x1a);
		break;
	case livekit::ConnectionQuality::Poor:
		bars = 1;
		color = QColor(0xe6, 0x7e, 0x22);
		break;
	case livekit::ConnectionQuality::Lost:
		bars = 1;
		color = QColor(0xf5, 0x3f, 0x3f);
		break;
	case livekit::ConnectionQuality::Unknown:
		break;
	}

	p.setPen(Qt::NoPen);
	p.setBrush(color);
	if (bars >= 1) p.drawRect(bx, by + 6, 2, 4);
	if (bars >= 2) p.drawRect(bx + 4, by + 3, 2, 7);
	if (bars >= 3) p.drawRect(bx + 8, by, 2, 10);

	if (_isVideoStreamPaused) {
		const QRect pausedRect(bx + 16, by - 3, 54, 16);
		p.setBrush(QColor(0xe6, 0x7e, 0x22, 220));
		p.drawRoundedRect(pausedRect, 5, 5);
		p.setPen(Qt::white);
		p.setFont(QFont("Microsoft YaHei", 8, QFont::DemiBold));
		p.drawText(pausedRect, Qt::AlignCenter, QCoreApplication::translate("MeetingUI", "Paused: Network"));
	}

	if (_isPinned) {
		p.setFont(QFont("Segoe UI Emoji", 10));
		p.setPen(Qt::white);
		const int pinOffset = _isVideoStreamPaused ? 74 : 16;
		p.drawText(QRect(bx + pinOffset, by - 2, 16, 16), Qt::AlignCenter, QString::fromUtf8("📌"));
	}
	p.restore();
}

void VideoTileWidget::drawVideoPlaceholder(QPainter &p, const QRect &r) {
	using Error = livekit::TrackPublication::SubscriptionError;
	p.fillRect(r, QColor(0x14, 0x16, 0x1d));
	p.setPen(_isVideoStreamPaused || _videoSubscriptionError != Error::None
		? QColor(0xe6, 0x7e, 0x22) : QColor(0x86, 0x90, 0x9c));
	p.setFont(QFont("Microsoft YaHei", 12));
	QString label;
	switch (_videoSubscriptionError) {
	case Error::CodecUnsupported:
		label = QCoreApplication::translate("MeetingUI", "Cannot receive this video format");
		break;
	case Error::TrackNotFound:
		label = QCoreApplication::translate("MeetingUI", "Video is no longer available");
		break;
	case Error::Unknown:
		label = QCoreApplication::translate("MeetingUI", "Video subscription failed");
		break;
	case Error::None:
		label = _isVideoStreamPaused
			? QCoreApplication::translate("MeetingUI", "Video paused due to network congestion")
			: QCoreApplication::translate("MeetingUI", "Waiting for video...");
		break;
	}
	p.drawText(r.adjusted(12, 0, -12, 0), Qt::AlignCenter,
		p.fontMetrics().elidedText(label, Qt::ElideRight, std::max(0, r.width() - 24)));
}

void VideoTileWidget::setRemoteInputEnabled(bool enabled) {
    _remoteInputEnabled = enabled;
    if (_pinBtn) _pinBtn->setVisible(!enabled && (_isPinned || underMouse()));
    update();
}
QRectF VideoTileWidget::remoteControlContentRect() {
    std::lock_guard<std::mutex> lock(_frameMutex);
    if (!_isVideoActive || _isVideoStreamPaused || _currentFrame.isNull()) return {};
    const auto fitted = _currentFrame.size().scaled(size(),Qt::KeepAspectRatio);
    return QRectF((width()-fitted.width())/2,(height()-fitted.height())/2,fitted.width(),fitted.height());
}

void VideoTileWidget::drawVideoFrame(QPainter &p, const QRect &r) {

	QImage frameCopy;
	livekit::render::VideoRenderFrame::Ptr renderFrame;
	{
		std::lock_guard<std::mutex> lock(_frameMutex);
		frameCopy = _currentFrame;
		renderFrame = _currentRenderFrame;
	}

	if (_isVideoStreamPaused || frameCopy.isNull()) {
		drawVideoPlaceholder(p, r);
		return;
	}

	if (!_hasLoggedFirstPaint) {
		_hasLoggedFirstPaint = true;
		LogToConsole(LogCategory::WebRTC, "PAINT_FRAME", QStringLiteral("first_frame image=%1x%2 viewport=%3x%4")
			.arg(frameCopy.width()).arg(frameCopy.height()).arg(r.width()).arg(r.height()));
	}

	const auto paintStartedAt = std::chrono::steady_clock::now();
	p.fillRect(r, QColor(0x0e, 0x10, 0x14));

	QImage scaled = frameCopy.scaled(r.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
	const int x = r.x() + (r.width() - scaled.width()) / 2;
	const int y = r.y() + (r.height() - scaled.height()) / 2;
	p.drawImage(x, y, scaled);
	if (renderFrame && p.isActive()) {
		renderFrame->NotifyRenderStage(
			"qt_cpu_paint",
			std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - paintStartedAt));
		renderFrame->NotifyRendered("qt_cpu_paint");
	}
}

void VideoTileWidget::drawBottomNameTag(QPainter &p, const QRect &r) {
	if (!_isVideoActive &&
		_videoSubscriptionError == livekit::TrackPublication::SubscriptionError::None) return;

	const int tagH = 24;
	const int margin = 12;

	QFont font("Microsoft YaHei", 10);
	p.setFont(font);
	QFontMetrics fm(font);
	const auto label = fm.elidedText(_displayName, Qt::ElideRight, std::max(0, r.width() - margin * 2 - 30));
	const int textW = fm.horizontalAdvance(label);
	const int tagW = textW + 30;

	QRect tagRect(r.x() + margin, r.bottom() - margin - tagH, tagW, tagH);

	p.save();
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0, 0, 0, 160));
	p.drawRoundedRect(tagRect, 6, 6);

	const int micX = tagRect.x() + 10;
	const int micY = tagRect.center().y();
	p.setPen(QPen(_isAudioMuted ? QColor(0xf5, 0x3f, 0x3f) : QColor(0x00, 0xb4, 0x2a), 1.4, Qt::SolidLine, Qt::RoundCap));
	p.drawRoundedRect(_isScreenShare ? QRect(micX - 5, micY - 4, 10, 7) : QRect(micX - 3, micY - 4, 6, 7), 2, 2);
	p.drawLine(micX, micY + 3, micX, micY + 5);
	p.drawLine(micX - 3, micY + 5, micX + 3, micY + 5);
	if (_isAudioMuted && !_isScreenShare) {
		p.drawLine(micX - 4, micY - 5, micX + 4, micY + 6);
	}

	p.setPen(Qt::white);
	p.drawText(QRect(tagRect.x() + 20, tagRect.y(), textW + 6, tagH), Qt::AlignVCenter | Qt::AlignLeft, label);
	p.restore();
}

void VideoTileWidget::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		emit tileClicked();
	}
	Ui::RpWidget::mousePressEvent(e);
}

void VideoTileWidget::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		emit tileDoubleClicked();
	}
	Ui::RpWidget::mouseDoubleClickEvent(e);
}

// ----------------------------------------------------
// RoomTopBarWidget 实现
// ----------------------------------------------------

RoomTopBarWidget::RoomTopBarWidget(QWidget *parent)
	: Ui::RpWidget(parent) {
	setMinimumHeight(44);
	setMouseTracking(true);
	setAttribute(Qt::WA_OpaquePaintEvent, false);
#if COHAVORA_SHOW_CONSOLE_BUTTON
	_uiaConsole = new QPushButton(this);
	_uiaConsole->setObjectName(QStringLiteral("meetingConsole"));
	_uiaConsole->setAccessibleName(QCoreApplication::translate("MeetingUI", "Console"));
	_uiaConsole->setFlat(true);
	_uiaConsole->setAttribute(Qt::WA_TransparentForMouseEvents);
	_uiaConsole->setStyleSheet(QStringLiteral("QPushButton { background: transparent; border: 0; }"));
	connect(_uiaConsole, &QPushButton::clicked, this, [this] { _consoleStream.fire({}); });
#endif
	_uiaTelemetry = new QPushButton(this);
	_uiaTelemetry->setObjectName(QStringLiteral("meetingTelemetry"));
	_uiaTelemetry->setAccessibleName(QCoreApplication::translate("MeetingUI", "Meeting telemetry"));
	_uiaTelemetry->setFlat(true);
	_uiaTelemetry->setAttribute(Qt::WA_TransparentForMouseEvents);
	_uiaTelemetry->setStyleSheet(QStringLiteral("QPushButton { background: transparent; border: 0; }"));
	connect(_uiaTelemetry, &QPushButton::clicked, this, [this] {
		emit telemetryDetailsRequested();
	});
}

void RoomTopBarWidget::updateDuration(int seconds) {
	_durationSeconds = seconds;
	update();
}

void RoomTopBarWidget::setActiveSpeaker(const QString &speakerName) {
	_speakerName = speakerName;
	setToolTip(QCoreApplication::translate("MeetingUI", "Speaking: %1").arg(speakerName));
	update();
}

void RoomTopBarWidget::setMeetingId(const QString &meetingId) {
	_meetingId = meetingId;
	if (auto *parent = parentWidget()) {
		QCoreApplication::postEvent(parent, new QResizeEvent(parent->size(), parent->size()));
	}
	QResizeEvent ev(size(), size());
	resizeEvent(&ev);
	update();
}

void RoomTopBarWidget::setTelemetrySnapshot(const QVariantMap &snapshot) {
	if (!snapshot.isEmpty() && !_telemetrySnapshot.isEmpty() &&
		snapshot.value(QStringLiteral("sessionGeneration")).toULongLong() ==
			_telemetrySnapshot.value(QStringLiteral("sessionGeneration")).toULongLong() &&
		snapshot.value(QStringLiteral("revision")).toULongLong() <
			_telemetrySnapshot.value(QStringLiteral("revision")).toULongLong()) {
		return;
	}
	_telemetrySnapshot = snapshot;
	update();
}

int RoomTopBarWidget::heightForWidth(int width) const {
 const QFontMetrics metrics(QFont("Microsoft YaHei", 9));
 const int toolsWidth = metrics.horizontalAdvance(QCoreApplication::translate("MeetingUI", "Picture-in-Picture"))
#if COHAVORA_SHOW_CONSOLE_BUTTON
     + metrics.horizontalAdvance(QCoreApplication::translate("MeetingUI", "Console 📋"))
#endif
     + metrics.horizontalAdvance(QCoreApplication::translate("MeetingUI", "🐛 Simulate")) + 80;
 const int idWidth = metrics.horizontalAdvance(QCoreApplication::translate("MeetingUI", "🆔 Meeting ID: %1 📋").arg(_meetingId)) + 24;
 return width < toolsWidth + idWidth + 420 ? 88 : 44;
}

void RoomTopBarWidget::resizeEvent(QResizeEvent *e) {
	const int w = width();
	const int h = height();
	const int btnW = 38;

	_closeRect = QRect(w - btnW, 0, btnW, h);
	_maxRect = QRect(w - btnW * 2, 0, btnW, h);
	_minRect = QRect(w - btnW * 3, 0, btnW, h);

	int rightX = w - btnW * 3 - 6;

 const bool twoRows = heightForWidth(w) > 44;
 const int rowY = twoRows ? 52 : 8;
 const QFontMetrics metrics(QFont("Microsoft YaHei", 9));
 const auto buttonWidth = [&](const QString &text) { return metrics.horizontalAdvance(text) + 24; };
 const int simulateW = buttonWidth(QCoreApplication::translate("MeetingUI", "🐛 Simulate"));
 const int layoutW = std::max(buttonWidth(QCoreApplication::translate("MeetingUI", "Grid View")),
     buttonWidth(QCoreApplication::translate("MeetingUI", "Picture-in-Picture"))) + 8;
 _simulateRect = QRect(rightX - simulateW, rowY, simulateW, 28);
 rightX -= simulateW + 4;
#if COHAVORA_SHOW_CONSOLE_BUTTON
 const int consoleW = buttonWidth(QCoreApplication::translate("MeetingUI", "Console 📋"));
 _consoleRect = QRect(rightX - consoleW, rowY, consoleW, 28);
	_uiaConsole->setGeometry(_consoleRect);
 rightX -= consoleW + 4;
#else
 _consoleRect = QRect();
#endif
 _layoutRect = QRect(rightX - layoutW, rowY, layoutW, 28);
 rightX -= layoutW + 4;
 const int leftInfoRight = QFontMetrics(QFont("Microsoft YaHei", 10)).horizontalAdvance(
     QCoreApplication::translate("MeetingUI", "Meetings")) + 116;
 _qualityRect = QRect(leftInfoRight - 22, 8, 20, 28);
	_uiaTelemetry->setGeometry(_qualityRect);
 if (!_meetingId.isEmpty()) {
  auto idFont = QFont("Microsoft YaHei", 9); idFont.setBold(true);
  const int desired = QFontMetrics(idFont).horizontalAdvance(
      QCoreApplication::translate("MeetingUI", "🆔 Meeting ID: %1 📋").arg(_meetingId)) + 24;
  const int available = (twoRows ? w - btnW * 3 - 12 : rightX) - leftInfoRight - 8;
  _meetingIdRect = QRect(leftInfoRight, 9, std::max(1, std::min(desired, available)), 26);
 } else {
  _meetingIdRect = QRect();
 }
 const int leftBoundary = twoRows ? 8 : (_meetingId.isEmpty() ? leftInfoRight : _meetingIdRect.right() + 8);
	const int rightButtonsLeft = rightX;
	const int availCenterW = rightButtonsLeft - leftBoundary - 16;

	if (availCenterW >= 180) {
		const int pillW = std::min(availCenterW, 200);
		int pillX = (w - pillW) / 2;
		pillX = std::max(leftBoundary + 8, std::min(pillX, rightButtonsLeft - 8 - pillW));
		_speakerCapsuleRect = QRect(pillX, (twoRows ? 53 : 9), pillW, 26);
	} else if (availCenterW >= 110) {
		const int pillW = availCenterW;
		const int pillX = leftBoundary + 8;
		_speakerCapsuleRect = QRect(pillX, (twoRows ? 53 : 9), pillW, 26);
	} else {
		_speakerCapsuleRect = QRect(); // 窗口空间不足时自动隐藏，彻底杜绝元素重叠
	}
}

void RoomTopBarWidget::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);

	const int w = width();
	const int h = height();

	p.fillRect(rect(), QColor(0xfd, 0xfd, 0xfe));
	p.setPen(QColor(0xeb, 0xed, 0xf0));
	p.drawLine(0, h - 1, w, h - 1);

	// 1. 左侧：Logo、会议名称与持续时间
	p.save();
	const int logoX = 14;
	const int logoY = 22;
	windowIcon().paint(&p, QRect(logoX - 8, logoY - 12, 24, 24));

	QFont font("Microsoft YaHei", 10);
	p.setFont(font);
	p.setPen(QColor(0x4e, 0x59, 0x69));
	const int nameWidth = QFontMetrics(font).horizontalAdvance(QCoreApplication::translate("MeetingUI", "Meetings"));
	p.drawText(QRect(logoX + 16, 0, nameWidth, 44), Qt::AlignVCenter | Qt::AlignLeft, QCoreApplication::translate("MeetingUI", "Meetings"));

	const int minutes = _durationSeconds / 60;
	const int secs = _durationSeconds % 60;
	const QString timeStr = QString("%1:%2")
		.arg(minutes, 2, 10, QChar('0'))
		.arg(secs, 2, 10, QChar('0'));
	
	QFont timeFont("Microsoft YaHei", 10, QFont::DemiBold);
	p.setFont(timeFont);
	p.setPen(QColor(0x1f, 0x23, 0x29));
	p.drawText(QRect(logoX + 24 + nameWidth, 0, 60, 44), Qt::AlignVCenter | Qt::AlignLeft, timeStr);

	const QString availability = _telemetrySnapshot.value(
		QStringLiteral("availability"), QStringLiteral("UNKNOWN")).toString();
	QColor qualityColor(0x86, 0x90, 0x9c);
	if (availability == QStringLiteral("VALID")) qualityColor = QColor(0x00, 0x9a, 0x29);
	else if (availability == QStringLiteral("WARMING_UP")) qualityColor = QColor(0x16, 0x77, 0xff);
	else if (availability == QStringLiteral("STALE")) qualityColor = QColor(0xd4, 0x88, 0x06);
	else if (availability == QStringLiteral("TIMEOUT") ||
		availability == QStringLiteral("INVALID")) qualityColor = QColor(0xd9, 0x36, 0x3e);
	else if (availability == QStringLiteral("UNSUPPORTED")) qualityColor = QColor(0xa6, 0x1d, 0x24);
	if (_hoverBtn == HoverBtn::Quality) {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0xf2, 0xf3, 0xf5));
		p.drawRoundedRect(_qualityRect, 4, 4);
	}
	const double coverage = _telemetrySnapshot.value(QStringLiteral("coverage"), 0.0).toDouble();
	const int activeBars = availability == QStringLiteral("VALID")
		? (coverage >= 0.66 ? 3 : (coverage >= 0.33 ? 2 : 1))
		: (availability == QStringLiteral("WARMING_UP") ? 1 : 0);
	const int sigX = _qualityRect.left() + 4;
	const int sigY = _qualityRect.center().y() + 5;
	p.setPen(Qt::NoPen);
	for (int bar = 0; bar != 3; ++bar) {
		p.setBrush(bar < activeBars ? qualityColor : QColor(0xd8, 0xdc, 0xe3));
		const int barHeight = 4 + bar * 3;
		p.drawRect(sigX + bar * 4, sigY - barHeight, 2, barHeight);
	}

	p.restore();

	// 1.5 会议号胶囊徽标与点击复制
	if (!_meetingIdRect.isEmpty() && !_meetingId.isEmpty()) {
		p.save();
		bool isHover = _meetingIdRect.contains(mapFromGlobal(QCursor::pos()));
		p.setPen(QPen(_copiedAnim ? QColor(0x52, 0xc4, 0x1a) : (isHover ? QColor(0x16, 0x77, 0xff) : QColor(0xd9, 0xd9, 0xd9)), 1));
		p.setBrush(_copiedAnim ? QColor(0xf6, 0xff, 0xed) : (isHover ? QColor(0xf0, 0xf5, 0xff) : QColor(0xf5, 0xf7, 0xfa)));
		p.drawRoundedRect(_meetingIdRect, 6, 6);

		QFont mFont("Microsoft YaHei", 9);
		mFont.setBold(true);
		p.setFont(mFont);
		p.setPen(_copiedAnim ? QColor(0x52, 0xc4, 0x1a) : (isHover ? QColor(0x16, 0x77, 0xff) : QColor(0x4e, 0x59, 0x69)));
		QString dispText = _copiedAnim ? QCoreApplication::translate("MeetingUI", "✔ Meeting ID Copied") : QCoreApplication::translate("MeetingUI", "🆔 Meeting ID: %1 📋").arg(_meetingId);
		p.drawText(_meetingIdRect, Qt::AlignCenter, dispText);
		p.restore();
	}

	// 2. 中间：正在讲话提示胶囊
	if (!_speakerCapsuleRect.isEmpty()) {
		p.save();
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0xe8, 0xf3, 0xff));
		p.drawRoundedRect(_speakerCapsuleRect, 6, 6);

		QFont speakerFont("Microsoft YaHei", 9);
		p.setFont(speakerFont);
		p.setPen(QColor(0x16, 0x77, 0xff));
		QString speakerText = _speakerName.isEmpty() ? QCoreApplication::translate("MeetingUI", "Speaking: None") : QCoreApplication::translate("MeetingUI", "Speaking: %1").arg(_speakerName);
		QFontMetrics fm(speakerFont);
		QString elided = fm.elidedText(speakerText, Qt::ElideMiddle, _speakerCapsuleRect.width() - 12);
		p.drawText(_speakerCapsuleRect, Qt::AlignCenter, elided);
		p.restore();
	}

	// 3. 右侧工具按钮
	auto drawTextBtn = [&](const QRect &r, const QString &text, bool hovered, bool hasArrow = false, const QColor &customColor = QColor(0x4e, 0x59, 0x69)) {
		p.save();
		if (hovered) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0xf2, 0xf3, 0xf5));
			p.drawRoundedRect(r, 6, 6);
		}
		p.setFont(QFont("Microsoft YaHei", 9));
		p.setPen(customColor);
		if (hasArrow) {
			p.drawText(r.adjusted(4, 0, -12, 0), Qt::AlignCenter, text);
			p.setPen(QPen(QColor(0x86, 0x90, 0x9c), 1.3));
			const int ax = r.right() - 10;
			const int ay = r.center().y();
			p.drawLine(ax - 3, ay - 1, ax, ay + 2);
			p.drawLine(ax, ay + 2, ax + 3, ay - 1);
		} else {
			p.drawText(r, Qt::AlignCenter, text);
		}
		p.restore();
	};

	QString layoutStr = (_currentViewMode == VideoViewMode::Grid) ? QCoreApplication::translate("MeetingUI", "Grid View") : QCoreApplication::translate("MeetingUI", "Picture-in-Picture");
	drawTextBtn(_layoutRect, layoutStr, _hoverBtn == HoverBtn::Layout, true);
#if COHAVORA_SHOW_CONSOLE_BUTTON
	drawTextBtn(_consoleRect, QCoreApplication::translate("MeetingUI", "Console 📋"), _hoverBtn == HoverBtn::Console, false, QColor(0x16, 0x77, 0xff));
#endif
	drawTextBtn(_simulateRect, QCoreApplication::translate("MeetingUI", "🐛 Simulate"), _hoverBtn == HoverBtn::Simulate, false, QColor(0xe6, 0x7e, 0x22));

	// 4. 窗口控制按钮
	p.save();
	if (_hoverBtn == HoverBtn::Min) p.fillRect(_minRect, QColor(0xe5, 0xe8, 0xef));
	if (_hoverBtn == HoverBtn::Max) p.fillRect(_maxRect, QColor(0xe5, 0xe8, 0xef));
	if (_hoverBtn == HoverBtn::Close) p.fillRect(_closeRect, QColor(0xf5, 0x3f, 0x3f));

	p.setPen(QPen((_hoverBtn == HoverBtn::Close) ? Qt::white : QColor(0x60, 0x62, 0x66), 1.2));
	p.drawLine(_minRect.center().x() - 5, _minRect.center().y(), _minRect.center().x() + 5, _minRect.center().y());
	p.drawRect(_maxRect.center().x() - 5, _maxRect.center().y() - 5, 10, 10);
	p.drawLine(_closeRect.center().x() - 5, _closeRect.center().y() - 5, _closeRect.center().x() + 5, _closeRect.center().y() + 5);
	p.drawLine(_closeRect.center().x() + 5, _closeRect.center().y() - 5, _closeRect.center().x() - 5, _closeRect.center().y() + 5);
	p.restore();
}

void RoomTopBarWidget::mouseMoveEvent(QMouseEvent *e) {
	const QPoint pos = e->pos();
	HoverBtn next = HoverBtn::None;

	if (_closeRect.contains(pos)) next = HoverBtn::Close;
	else if (_maxRect.contains(pos)) next = HoverBtn::Max;
	else if (_minRect.contains(pos)) next = HoverBtn::Min;
	else if (_qualityRect.contains(pos)) next = HoverBtn::Quality;
	else if (_simulateRect.contains(pos)) next = HoverBtn::Simulate;
	else if (_consoleRect.contains(pos)) next = HoverBtn::Console;
	else if (_layoutRect.contains(pos)) next = HoverBtn::Layout;

	if (!_meetingIdRect.isEmpty() && _meetingIdRect.contains(pos)) {
		setCursor(Qt::PointingHandCursor);
		setToolTip(QCoreApplication::translate("MeetingUI", "Click to copy meeting ID: %1").arg(_meetingId));
	} else if (next == HoverBtn::Quality) {
		setCursor(Qt::PointingHandCursor);
		const auto availability = _telemetrySnapshot.value(
			QStringLiteral("availability"), QStringLiteral("UNKNOWN")).toString();
		const auto age = _telemetrySnapshot.value(
			QStringLiteral("sampleAgeMs"), -1).toLongLong();
		QString tooltip = age >= 0
			? QCoreApplication::translate("MeetingUI", "Telemetry: %1, age %2 ms")
				.arg(LocalizeTelemetryDisplayText(availability)).arg(age)
			: QCoreApplication::translate("MeetingUI", "Telemetry: %1")
				.arg(LocalizeTelemetryDisplayText(availability));
		const auto reconnectVideo = _telemetrySnapshot.value(
			QStringLiteral("reconnectVideoAvailability"), QStringLiteral("UNKNOWN")).toString();
		if (reconnectVideo == QStringLiteral("WARMING_UP") ||
			reconnectVideo == QStringLiteral("TIMEOUT")) {
			tooltip += QStringLiteral("\n") +
				QCoreApplication::translate("MeetingUI", "Video recovery: %1 (%2/%3 stable)")
					.arg(LocalizeTelemetryDisplayText(reconnectVideo),
						 _telemetrySnapshot.value(QStringLiteral("reconnectVideoRecovered")).toString(),
						 _telemetrySnapshot.value(QStringLiteral("reconnectVideoExpected")).toString());
		}
		setToolTip(tooltip);
	} else if (next != HoverBtn::None) {
		setCursor(Qt::PointingHandCursor);
		setToolTip(QString());
	} else {
		setCursor(Qt::ArrowCursor);
		setToolTip(QString());
	}

	if (next != _hoverBtn) {
		_hoverBtn = next;
		update();
	}
}

bool RoomTopBarWidget::isWindowDragArea(const QPoint &position) const {
	return rect().contains(position) &&
		!_qualityRect.contains(position) && !_meetingIdRect.contains(position) &&
		!_minRect.contains(position) && !_maxRect.contains(position) &&
		!_closeRect.contains(position) && !_consoleRect.contains(position) &&
		!_simulateRect.contains(position) && !_layoutRect.contains(position);
}

void RoomTopBarWidget::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		if (!_meetingIdRect.isEmpty() && _meetingIdRect.contains(e->pos())) {
			if (!_meetingId.isEmpty()) {
				QApplication::clipboard()->setText(_meetingId);
				_copiedAnim = true;
				update();
				QTimer::singleShot(1800, this, [this]() {
					_copiedAnim = false;
					update();
				});
			}
			return;
		}
		if (_qualityRect.contains(e->pos())) {
			emit telemetryDetailsRequested();
		} else if (_minRect.contains(e->pos())) {
			_minStream.fire({});
		} else if (_maxRect.contains(e->pos())) {
			_maxStream.fire({});
		} else if (_closeRect.contains(e->pos())) {
			_closeStream.fire({});
		} else if (_consoleRect.contains(e->pos())) {
			_consoleStream.fire({});
		} else if (_simulateRect.contains(e->pos())) {
			showSimulateScenarioMenu(mapToGlobal(QPoint(_simulateRect.left(), _simulateRect.bottom() + 4)));
		} else if (_layoutRect.contains(e->pos())) {
			_currentViewMode = (_currentViewMode == VideoViewMode::Grid) ? VideoViewMode::Pip : VideoViewMode::Grid;
			_viewModeStream.fire_copy(_currentViewMode);
			update();
		} else {
			emit windowDragRequested();
			e->accept();
			return;
		}
	}
	Ui::RpWidget::mousePressEvent(e);
}

void RoomTopBarWidget::showSimulateScenarioMenu(const QPoint &globalPos) {
	QMenu menu(this);
	AppTheme::styleMenu(menu, AppTheme::Tone::Dark);

	QAction *header = menu.addAction(QCoreApplication::translate("MeetingUI", "Simulate Scenario"));
	header->setEnabled(false);
	menu.addSeparator();

	struct ScenarioEntry {
		QString name;
		livekit::SimulateScenarioType type;
	};

	const std::vector<ScenarioEntry> entries = {
		{ QCoreApplication::translate("MeetingUI", "Signal reconnect"), livekit::SimulateScenarioType::SignalReconnect },
		{ QCoreApplication::translate("MeetingUI", "Full reconnect"), livekit::SimulateScenarioType::FullReconnect },
		{ QCoreApplication::translate("MeetingUI", "Speaker update"), livekit::SimulateScenarioType::SpeakerUpdate },
		{ QCoreApplication::translate("MeetingUI", "Node failure"), livekit::SimulateScenarioType::NodeFailure },
		{ QCoreApplication::translate("MeetingUI", "Server migration"), livekit::SimulateScenarioType::Migration },
		{ QCoreApplication::translate("MeetingUI", "Server disconnect"), livekit::SimulateScenarioType::ServerLeave },
		{ QCoreApplication::translate("MeetingUI", "Switch ICE candidate"), livekit::SimulateScenarioType::SwitchCandidate },
		{ QCoreApplication::translate("MeetingUI", "Rotate E2EE key"), livekit::SimulateScenarioType::E2eeKeyRatchet },
		{ QCoreApplication::translate("MeetingUI", "Update participant name"), livekit::SimulateScenarioType::ParticipantName },
		{ QCoreApplication::translate("MeetingUI", "Update participant metadata"), livekit::SimulateScenarioType::ParticipantMetadata },
		{ QCoreApplication::translate("MeetingUI", "Clear simulation"), livekit::SimulateScenarioType::Clear }
	};

	for (const auto &entry : entries) {
		QAction *act = menu.addAction(entry.name);
		connect(act, &QAction::triggered, [this, type = entry.type] {
			_simulateScenarioStream.fire_copy(type);
		});
	}

	menu.exec(globalPos);
}

void RoomTopBarWidget::leaveEventHook(QEvent *e) {
	_hoverBtn = HoverBtn::None;
	update();
	Ui::RpWidget::leaveEventHook(e);
}

// ----------------------------------------------------
// RoomBottomBarWidget 实现
// ----------------------------------------------------

RoomBottomBarWidget::RoomBottomBarWidget(QWidget *parent)
	: Ui::RpWidget(parent) {
	setMinimumHeight(kBottomBarHeight);
	setMouseTracking(true);
	setAttribute(Qt::WA_OpaquePaintEvent, false);

	_chatInput = new QLineEdit(this);
	_chatInput->setPlaceholderText(QCoreApplication::translate("MeetingUI", "Type a message..."));
	MeetingUI::AppTheme::setStyleVariant(*_chatInput, "meeting-room-window-chatinput");

	_handBtn = new QPushButton(QString::fromUtf8("✋"), this);
	_handBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Raise Hand to Speak"));
	_handBtn->setFixedSize(32, 32);
	MeetingUI::AppTheme::setStyleVariant(*_handBtn, "meeting-room-window-handbtn");

	connect(_chatInput, &QLineEdit::returnPressed, [this] {
		if (!_chatInput->text().trimmed().isEmpty()) {
			_sendChatStream.fire_copy(_chatInput->text().trimmed());
			_chatInput->clear();
		}
	});

	connect(_handBtn, &QPushButton::clicked, [this] {
		QMessageBox::information(this, QCoreApplication::translate("MeetingUI", "Raise Hand"), QCoreApplication::translate("MeetingUI", "The host has been notified that you would like to speak."));
	});

	_chatInput->hide();
	_handBtn->hide();
	const auto accessibleTool = [this](const char *id, const QString &name) {
		auto *button = new QPushButton(this);
		button->setObjectName(QString::fromLatin1(id));
		button->setAccessibleName(name);
		button->setFlat(true);
		button->setAttribute(Qt::WA_TransparentForMouseEvents);
		button->setStyleSheet(QStringLiteral("QPushButton { background: transparent; border: 0; }"));
		return button;
	};
	_uiaParticipants = accessibleTool("meetingParticipants", QCoreApplication::translate("MeetingUI", "Participants"));
	_uiaChat = accessibleTool("meetingChat", QCoreApplication::translate("MeetingUI", "Chat"));
	_uiaWhiteboard = accessibleTool("meetingWhiteboard", QCoreApplication::translate("MeetingUI", "Whiteboard"));
	_uiaAudio = accessibleTool("meetingMicrophone", QCoreApplication::translate("MeetingUI", "Microphone"));
	_uiaVideo = accessibleTool("meetingCamera", QCoreApplication::translate("MeetingUI", "Camera"));
	_uiaShare = accessibleTool("meetingShareScreen", QCoreApplication::translate("MeetingUI", "Share Screen"));
	_uiaEnd = accessibleTool("meetingLeave", QCoreApplication::translate("MeetingUI", "Leave Meeting"));
	for (auto *button : {_uiaAudio, _uiaVideo, _uiaShare}) button->setCheckable(true);
	_uiaAudio->setChecked(!_audioMuted);
	_uiaVideo->setChecked(_videoEnabled);
	connect(_uiaParticipants, &QPushButton::clicked, this, [this] { _participantsStream.fire({}); });
	connect(_uiaChat, &QPushButton::clicked, this, [this] { _chatStream.fire({}); });
	connect(_uiaWhiteboard, &QPushButton::clicked, this, [this] { _whiteboardStream.fire({}); });
	// Qt 5's accessibility Toggle action calls QAbstractButton::toggle(),
	// which emits toggled but not clicked. Commit the same business action for
	// UIA and ordinary activation. State projection updates the model first,
	// so its setChecked() notification must not submit another action.
	connect(_uiaAudio, &QPushButton::toggled, this, [this](bool enabled) {
		if (enabled == !_audioMuted) return;
		// UIA Toggle enters synchronously through COM. Device enumeration and
		// dialogs must run after that call returns, on the UI event loop.
		QMetaObject::invokeMethod(this, [this, enabled] {
			if (enabled != !_audioMuted) toggleAudio();
		}, Qt::QueuedConnection);
	});
	connect(_uiaVideo, &QPushButton::toggled, this, [this](bool enabled) {
		if (enabled == _videoEnabled) return;
		QMetaObject::invokeMethod(this, [this, enabled] {
			if (enabled != _videoEnabled) toggleVideo();
		}, Qt::QueuedConnection);
	});
	connect(_uiaShare, &QPushButton::toggled, this, [this](bool enabled) {
		if (enabled == canStopScreenShare()) return;
		QMetaObject::invokeMethod(this, [this, enabled] {
			if (!screenShareControlEnabled()) {
				_uiaShare->setChecked(canStopScreenShare());
				return;
			}
			if (enabled != canStopScreenShare()) _shareScreenStream.fire({});
		}, Qt::QueuedConnection);
	});
	connect(_uiaEnd, &QPushButton::clicked, this, [this] { _endMeetingStream.fire({}); });
}

void RoomBottomBarWidget::setAudioMuted(bool muted) {
	cancelDeviceAction(DeviceKind::Microphone);
	_audioMuted = muted;
	_uiaAudio->setEnabled(!_inRecovery);
	_uiaAudio->setChecked(!muted);
	_uiaAudio->setAccessibleName(QCoreApplication::translate("MeetingUI", muted ? "Unmute" : "Mute"));
	update();
}

void RoomBottomBarWidget::setSpeakerMuted(bool muted) {
	cancelDeviceAction(DeviceKind::Speaker);
	_speakerMuted = muted;
	update();
}

void RoomBottomBarWidget::setVideoEnabled(bool enabled) {
	cancelDeviceAction(DeviceKind::Camera);
	_videoEnabled = enabled;
	_uiaVideo->setEnabled(!_inRecovery);
	_uiaVideo->setChecked(enabled);
	_uiaVideo->setAccessibleName(QCoreApplication::translate("MeetingUI", enabled ? "Stop Video" : "Start Video"));
	update();
}

void RoomBottomBarWidget::setScreenShareState(livekit::ScreenShareState state) {
	_screenShareState = state;
	_uiaShare->setChecked(canStopScreenShare());
	_uiaShare->setAccessibleName(QCoreApplication::translate("MeetingUI",
		canStopScreenShare() ? "Stop Sharing" : "Share Screen"));
	_uiaShare->setEnabled(screenShareControlEnabled());
	update();
}

void RoomBottomBarWidget::setScreenShareAvailable(bool available) {
	_screenShareAvailable = available;
	_uiaShare->setEnabled(screenShareControlEnabled());
	update();
}

void RoomBottomBarWidget::cancelDeviceAction(DeviceKind kind) {
    auto &pending = _pendingDeviceActions[static_cast<std::size_t>(kind)];
    if (pending.callbacks) pending.callbacks->Revoke();
    pending = {};
}

void RoomBottomBarWidget::requestDeviceEnable(DeviceKind kind) {
    if (_inRecovery) return;
    auto &pending = _pendingDeviceActions[static_cast<std::size_t>(kind)];
    if (pending.callbacks) return;
    auto gate = OpenMeeting::QtCallbackGate<RoomBottomBarWidget>::Create(this);
    pending.callbacks = gate;
    if (kind == DeviceKind::Microphone) {
        _uiaAudio->setChecked(!_audioMuted);
        _uiaAudio->setEnabled(false);
    } else if (kind == DeviceKind::Camera) {
        _uiaVideo->setChecked(_videoEnabled);
        _uiaVideo->setEnabled(false);
    }
    const auto complete = [gate, kind](bool available) {
        gate->Post([kind, available](RoomBottomBarWidget *bar) {
            bar->cancelDeviceAction(kind);
            if (bar->_inRecovery) return;
            if (kind == DeviceKind::Microphone) bar->_uiaAudio->setEnabled(true);
            if (kind == DeviceKind::Camera) bar->_uiaVideo->setEnabled(true);
            if (!available) {
                const bool camera = kind == DeviceKind::Camera;
                const bool speaker = kind == DeviceKind::Speaker;
                ShowMeetingWarning(bar,
                    camera ? "meetingCameraUnavailable" : speaker ? "meetingSpeakerUnavailable" : "meetingMicrophoneUnavailable",
                    camera ? "meetingCameraUnavailableDismiss" : speaker ? "meetingSpeakerUnavailableDismiss" : "meetingMicrophoneUnavailableDismiss",
                    QCoreApplication::translate("MeetingUI", camera ? "Camera Unavailable" : speaker ? "Speaker Unavailable" : "Microphone Unavailable"),
                    QCoreApplication::translate("MeetingUI", camera ? "No camera device is available. Video cannot be enabled." : speaker ?
                        "No speaker output device is available. The speaker cannot be enabled." :
                        "No microphone input device is available. The microphone cannot be enabled."));
                return;
            }
            if (kind == DeviceKind::Camera) {
                bar->setVideoEnabled(true);
                bar->_toggleVideoStream.fire_copy(true);
            } else if (kind == DeviceKind::Microphone) {
                bar->setAudioMuted(false);
                bar->_toggleAudioStream.fire_copy(false);
            } else {
                bar->setSpeakerMuted(false);
                bar->_toggleSpeakerStream.fire_copy(false);
            }
        });
    };
    if (kind == DeviceKind::Camera) {
        auto &discovery = _cameraDiscovery ? *_cameraDiscovery : CameraDeviceDiscovery::Instance();
        pending.subscription = discovery.request([complete](const auto &result) {
            complete(result.succeeded && std::any_of(result.devices->begin(), result.devices->end(),
                [](const auto &device) { return !device.path.empty(); }));
        });
    } else {
        auto &discovery = kind == DeviceKind::Speaker
            ? (_speakerDiscovery ? *_speakerDiscovery : SpeakerDeviceDiscovery())
            : (_microphoneDiscovery ? *_microphoneDiscovery : MicrophoneDeviceDiscovery());
        pending.subscription = discovery.request([complete](const auto &result) {
            complete(result.succeeded && std::any_of(result.devices->begin(), result.devices->end(),
                [](const auto &device) { return !device.id.empty(); }));
        });
    }
}

void RoomBottomBarWidget::toggleAudio() {
    if (_inRecovery) { _uiaAudio->setChecked(!_audioMuted); return; }
    if (_audioMuted) { requestDeviceEnable(DeviceKind::Microphone); return; }
    setAudioMuted(true);
    _toggleAudioStream.fire_copy(true);
}

void RoomBottomBarWidget::toggleVideo() {
    if (_inRecovery) { _uiaVideo->setChecked(_videoEnabled); return; }
    if (!_videoEnabled) { requestDeviceEnable(DeviceKind::Camera); return; }
    setVideoEnabled(false);
    _toggleVideoStream.fire_copy(false);
}

void RoomBottomBarWidget::toggleSpeaker() {
    if (_inRecovery) return;
    if (_speakerMuted) { requestDeviceEnable(DeviceKind::Speaker); return; }
    setSpeakerMuted(true);
    _toggleSpeakerStream.fire_copy(true);
}

void RoomBottomBarWidget::hideEvent(QHideEvent *event) {
    for (auto kind : {DeviceKind::Microphone, DeviceKind::Speaker, DeviceKind::Camera}) cancelDeviceAction(kind);
    if (_deviceMenu) _deviceMenu->close();
    if (_uiaAudio) _uiaAudio->setEnabled(!_inRecovery);
    if (_uiaVideo) _uiaVideo->setEnabled(!_inRecovery);
    Ui::RpWidget::hideEvent(event);
}

void RoomBottomBarWidget::setParticipantCount(int count) {
	_participantCount = count;
	update();
}

void RoomBottomBarWidget::setChatUnreadCount(int count) {
	_chatUnreadCount = count;
	update();
}

void RoomBottomBarWidget::setInRecovery(bool inRecovery) {
	if (_inRecovery == inRecovery) return;
	_inRecovery = inRecovery;
    if (inRecovery) {
        for (auto kind : {DeviceKind::Microphone, DeviceKind::Speaker, DeviceKind::Camera}) cancelDeviceAction(kind);
        if (_deviceMenu) _deviceMenu->close();
    }
	for (auto *button : {_uiaParticipants, _uiaChat, _uiaWhiteboard, _uiaAudio, _uiaVideo})
		button->setEnabled(!inRecovery);
	_uiaShare->setEnabled(screenShareControlEnabled());
	update();
}

int RoomBottomBarWidget::heightForWidth(int width) const {
	Q_UNUSED(width);
	return kBottomBarHeight;
}

void RoomBottomBarWidget::resizeEvent(QResizeEvent *e) {
	Q_UNUSED(e);
	const int w = width();

	_toolItems = {
		{ 1, QCoreApplication::translate("MeetingUI", "Unmute"), QCoreApplication::translate("MeetingUI", "Mute"), QRect(), true },
		{ 11, QCoreApplication::translate("MeetingUI", "Enable Speaker"), QCoreApplication::translate("MeetingUI", "Speaker"), QRect(), true },
		{ 2, QCoreApplication::translate("MeetingUI", "Start Video"), QCoreApplication::translate("MeetingUI", "Stop Video"), QRect(), true },
		{ 3, QCoreApplication::translate("MeetingUI", "Share Screen"), QCoreApplication::translate("MeetingUI", "Share Screen"), QRect(), true },
		{ 4, QCoreApplication::translate("MeetingUI", "Invite"), QCoreApplication::translate("MeetingUI", "Invite"), QRect(), true },
		{ 5, QCoreApplication::translate("MeetingUI", "Participants (%1)").arg(_participantCount), QCoreApplication::translate("MeetingUI", "Participants (%1)").arg(_participantCount), QRect(), false },
		{ 6, QCoreApplication::translate("MeetingUI", "Chat"), QCoreApplication::translate("MeetingUI", "Chat"), QRect(), false },
		{ 7, QCoreApplication::translate("MeetingUI", "Whiteboard"), QCoreApplication::translate("MeetingUI", "Whiteboard"), QRect(), false },
		{ 10, QCoreApplication::translate("MeetingUI", "Simulate Scenario"), QCoreApplication::translate("MeetingUI", "Simulate Scenario"), QRect(), true },
	};

	const int endWidth = std::min(kBottomBarEndWidth, std::max(64, w / 5));
	_endMeetingRect = QRect(w - kBottomBarPadding - endWidth, 5, endWidth,
		kBottomBarHeight - 10);
	_uiaEnd->setGeometry(_endMeetingRect);
	const int controlsLeft = kBottomBarPadding;
	const int controlsRight = _endMeetingRect.left() - kBottomBarGap;
	const int controlsWidth = std::max(0, controlsRight - controlsLeft);
	const int itemCount = static_cast<int>(_toolItems.size());
	const int toolWidth = std::max(1, std::min(kBottomBarPreferredToolWidth,
		(controlsWidth - (itemCount - 1) * kBottomBarGap) / itemCount));
	const int toolsWidth = itemCount * toolWidth + (itemCount - 1) * kBottomBarGap;
	const int toolsLeft = controlsLeft + std::max(0, (controlsWidth - toolsWidth) / 2);
	_chatInput->hide();
	_handBtn->hide();
	for (size_t i = 0; i < _toolItems.size(); ++i) {
		_toolItems[i].rect = QRect(toolsLeft + static_cast<int>(i) * (toolWidth + kBottomBarGap),
			5, toolWidth, kBottomBarHeight - 10);
		switch (_toolItems[i].id) {
		case 1: _uiaAudio->setGeometry(_toolItems[i].rect); break;
		case 2: _uiaVideo->setGeometry(_toolItems[i].rect); break;
		case 3: _uiaShare->setGeometry(_toolItems[i].rect); break;
		case 5: _uiaParticipants->setGeometry(_toolItems[i].rect); break;
		case 6: _uiaChat->setGeometry(_toolItems[i].rect); break;
		case 7: _uiaWhiteboard->setGeometry(_toolItems[i].rect); break;
		default: break;
		}
	}
}

void RoomBottomBarWidget::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);

	const int w = width();
	const int h = height();

	p.fillRect(rect(), Qt::white);
	p.setPen(QColor(0xeb, 0xed, 0xf0));
	p.drawLine(0, 0, w, 0);

	for (const auto &item : _toolItems) {
		const QRect r = item.rect;
		const bool available = item.id == 3 ? screenShareControlEnabled() : !_inRecovery;
		const bool hovered = available && (_hoveredId == item.id);

		p.save();
		if (!available) {
			p.setOpacity(0.45);
		}
		if (hovered) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0xf2, 0xf3, 0xf5));
			p.drawRoundedRect(r, 8, 8);
		}

		const int cx = r.center().x();
		const int cy = r.y() + 18;

		if (item.id == 1) {
			const bool muted = _audioMuted;
			p.setPen(QPen(muted ? QColor(0xf5, 0x3f, 0x3f) : QColor(0x1f, 0x23, 0x29), 1.8, Qt::SolidLine, Qt::RoundCap));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(QRect(cx - 4, cy - 7, 8, 11), 3, 3);
			p.drawLine(cx, cy + 4, cx, cy + 8);
			p.drawLine(cx - 4, cy + 8, cx + 4, cy + 8);
			if (muted) {
				p.drawLine(cx - 7, cy - 8, cx + 7, cy + 9);
			}
		} else if (item.id == 11) {
			const bool muted = _speakerMuted;
			p.setPen(QPen(muted ? QColor(0xf5, 0x3f, 0x3f) : QColor(0x1f, 0x23, 0x29), 1.8, Qt::SolidLine, Qt::RoundCap));
			p.setBrush(Qt::NoBrush);

			// 喇叭后腔方块
			p.drawRoundedRect(QRect(cx - 7, cy - 3, 4, 7), 1, 1);
			// 喇叭扩音锥形
			QPainterPath hornPath;
			hornPath.moveTo(cx - 3, cy - 3);
			hornPath.lineTo(cx + 1, cy - 7);
			hornPath.lineTo(cx + 1, cy + 7);
			hornPath.lineTo(cx - 3, cy + 3);
			hornPath.closeSubpath();
			p.drawPath(hornPath);

			if (muted) {
				// 静音状态绘制红色斜线
				p.drawLine(cx - 8, cy - 8, cx + 8, cy + 9);
			} else {
				// 开启状态绘制两道流畅声波弧线
				p.drawArc(QRect(cx - 2, cy - 5, 8, 11), -55 * 16, 110 * 16);
				p.drawArc(QRect(cx - 3, cy - 8, 13, 17), -55 * 16, 110 * 16);
			}
		} else if (item.id == 2) {
			const bool closed = !_videoEnabled;
			p.setPen(QPen(closed ? QColor(0xf5, 0x3f, 0x3f) : QColor(0x1f, 0x23, 0x29), 1.8, Qt::SolidLine, Qt::RoundCap));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(QRect(cx - 8, cy - 6, 11, 12), 2, 2);
			QPainterPath camPath;
			camPath.moveTo(cx + 3, cy - 2);
			camPath.lineTo(cx + 8, cy - 6);
			camPath.lineTo(cx + 8, cy + 6);
			camPath.lineTo(cx + 3, cy + 2);
			camPath.closeSubpath();
			p.drawPath(camPath);
			if (closed) {
				p.drawLine(cx - 9, cy - 8, cx + 9, cy + 9);
			}
		} else if (item.id == 3) {
			p.setPen(QPen(QColor(0x00, 0xb4, 0x2a), 1.8, Qt::SolidLine, Qt::RoundCap));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(QRect(cx - 8, cy - 7, 16, 12), 2, 2);
			p.drawLine(cx, cy + 2, cx, cy - 3);
			p.drawLine(cx - 3, cy - 1, cx, cy - 4);
			p.drawLine(cx + 3, cy - 1, cx, cy - 4);
		} else if (item.id == 4) {
			p.setPen(QPen(QColor(0x1f, 0x23, 0x29), 1.6));
			p.drawEllipse(QPoint(cx - 3, cy - 4), 3, 3);
			p.drawArc(cx - 7, cy, 8, 8, 0, 180 * 16);
			p.setPen(QPen(QColor(0x16, 0x77, 0xff), 1.8));
			p.drawLine(cx + 4, cy - 2, cx + 8, cy - 2);
			p.drawLine(cx + 6, cy - 4, cx + 6, cy);
		} else if (item.id == 5) {
			p.setPen(QPen(QColor(0x1f, 0x23, 0x29), 1.6));
			p.drawEllipse(QPoint(cx - 3, cy - 4), 3, 3);
			p.drawArc(cx - 5, cy, 10, 8, 0, 180 * 16);
		} else if (item.id == 6) {
			p.setPen(QPen(QColor(0x1f, 0x23, 0x29), 1.6));
			p.drawRoundedRect(QRect(cx - 7, cy - 6, 14, 11), 3, 3);
			p.drawLine(cx - 3, cy - 2, cx + 3, cy - 2);
			p.drawLine(cx - 3, cy + 1, cx + 1, cy + 1);

			if (_chatUnreadCount > 0) {
				p.save();
				p.setPen(Qt::NoPen);
				p.setBrush(QColor(0xf5, 0x3f, 0x3f));
				if (_chatUnreadCount > 99) {
					p.drawRoundedRect(QRect(cx + 4, cy - 10, 16, 10), 5, 5);
					p.setFont(QFont("Microsoft YaHei", 6, QFont::Bold));
					p.setPen(Qt::white);
					p.drawText(QRect(cx + 4, cy - 10, 16, 10), Qt::AlignCenter, "99+");
				} else if (_chatUnreadCount > 9) {
					p.drawRoundedRect(QRect(cx + 4, cy - 10, 14, 10), 5, 5);
					p.setFont(QFont("Microsoft YaHei", 6, QFont::Bold));
					p.setPen(Qt::white);
					p.drawText(QRect(cx + 4, cy - 10, 14, 10), Qt::AlignCenter, QString::number(_chatUnreadCount));
				} else {
					p.drawEllipse(QPoint(cx + 8, cy - 6), 4, 4);
				}
				p.restore();
			}
		} else if (item.id == 7) {
			p.setPen(QPen(QColor(0x16, 0x77, 0xff), 1.6, Qt::SolidLine, Qt::RoundCap));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(QRect(cx - 9, cy - 7, 18, 13), 2, 2);
			p.drawLine(cx - 5, cy + 2, cx + 4, cy - 3);
			p.drawLine(cx, cy + 6, cx, cy + 10);
		} else if (item.id == 10) {
			const QColor bugCol = hovered ? QColor(0x16, 0x77, 0xff) : QColor(0x1f, 0x23, 0x29);
			p.setPen(QPen(bugCol, 1.6, Qt::SolidLine, Qt::RoundCap));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(QRect(cx - 5, cy - 4, 10, 11), 4, 4);
			p.drawArc(QRect(cx - 3, cy - 8, 6, 6), 0, 180 * 16);
			p.drawLine(cx - 2, cy - 7, cx - 5, cy - 10);
			p.drawLine(cx + 2, cy - 7, cx + 5, cy - 10);
			p.drawLine(cx - 5, cy - 2, cx - 9, cy - 4);
			p.drawLine(cx + 5, cy - 2, cx + 9, cy - 4);
			p.drawLine(cx - 5, cy + 2, cx - 10, cy + 2);
			p.drawLine(cx + 5, cy + 2, cx + 10, cy + 2);
			p.drawLine(cx - 5, cy + 6, cx - 9, cy + 8);
			p.drawLine(cx + 5, cy + 6, cx + 9, cy + 8);
			p.drawLine(cx, cy - 4, cx, cy + 7);
		}

		QString title = item.title;
		if (item.id == 1) title = _audioMuted ? QCoreApplication::translate("MeetingUI", "Unmute") : QCoreApplication::translate("MeetingUI", "Mute");
		else if (item.id == 11) title = _speakerMuted ? QCoreApplication::translate("MeetingUI", "Enable Speaker") : QCoreApplication::translate("MeetingUI", "Speaker");
		else if (item.id == 2) title = _videoEnabled ? QCoreApplication::translate("MeetingUI", "Stop Video") : QCoreApplication::translate("MeetingUI", "Start Video");
		else if (item.id == 3) {
			using State = livekit::ScreenShareState;
			if (_screenShareState == State::Starting) title = QCoreApplication::translate("MeetingUI", "Cancel Sharing");
			else if (_screenShareState == State::Active) title = QCoreApplication::translate("MeetingUI", "Stop Sharing");
			else if (_screenShareState == State::Stopping) title = QCoreApplication::translate("MeetingUI", "Stopping");
			else if (_screenShareState == State::StopFailed) title = QCoreApplication::translate("MeetingUI", "Retry Stop");
		}
		else if (item.id == 5) title = QCoreApplication::translate("MeetingUI", "Participants (%1)").arg(_participantCount);

		QFont font("Microsoft YaHei");
		font.setPixelSize(11);
		p.setFont(font);
		p.setPen(QColor(0x4e, 0x59, 0x69));
		const int textWidth = std::max(0, r.width() - (item.hasDropdown ? 18 : 8));
		const auto visibleTitle = QFontMetrics(font).elidedText(title, Qt::ElideRight, textWidth);
		p.drawText(QRect(r.x() + 4, r.y() + 32, r.width() - 8, r.height() - 32),
			Qt::AlignCenter | Qt::TextSingleLine, visibleTitle);

		if (item.hasDropdown) {
			p.setPen(QPen(QColor(0x86, 0x90, 0x9c), 1.4));
			const int ax = r.right() - 7;
			const int ay = r.y() + 10;
			p.drawLine(ax - 3, ay, ax, ay + 3);
			p.drawLine(ax, ay + 3, ax + 3, ay);
		}

		p.restore();
	}

	p.save();
	if (_endHovered) {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0xff, 0xec, 0xe8));
		p.drawRoundedRect(_endMeetingRect, 8, 8);
	}

	const int ecx = _endMeetingRect.center().x();
	const int ecy = _endMeetingRect.y() + 16;

	p.setPen(QPen(QColor(0xf5, 0x3f, 0x3f), 1.8, Qt::SolidLine, Qt::RoundCap));
	p.drawLine(ecx - 6, ecy - 7, ecx + 2, ecy - 7);
	p.drawLine(ecx + 2, ecy - 7, ecx + 2, ecy + 7);
	p.drawLine(ecx + 2, ecy + 7, ecx - 6, ecy + 7);

	p.drawLine(ecx - 8, ecy, ecx - 1, ecy);
	p.drawLine(ecx - 4, ecy - 3, ecx - 1, ecy);
	p.drawLine(ecx - 4, ecy + 3, ecx - 1, ecy);

	QFont endFont("Microsoft YaHei");
	endFont.setPixelSize(11);
	endFont.setBold(true);
	p.setFont(endFont);
	p.setPen(QColor(0xf5, 0x3f, 0x3f));
	const auto endMeetingTitle = QFontMetrics(endFont).elidedText(
		QCoreApplication::translate("MeetingUI", "End Meeting"), Qt::ElideRight,
		std::max(0, _endMeetingRect.width() - 8));
	p.drawText(QRect(_endMeetingRect.x() + 4, _endMeetingRect.y() + 32,
		_endMeetingRect.width() - 8, _endMeetingRect.height() - 32),
		Qt::AlignCenter | Qt::TextSingleLine, endMeetingTitle);
	p.restore();
}

QMenu *RoomBottomBarWidget::createDeviceMenu(const char *name) {
    if (_deviceMenu) _deviceMenu->close();
    auto *menu = new QMenu(this);
    menu->setObjectName(QString::fromLatin1(name));
    _deviceMenu = menu;
    AppTheme::styleMenu(*menu, AppTheme::Tone::Dark);
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    return menu;
}

void RoomBottomBarWidget::showAudioDeviceMenu(const QPoint &globalPos) {
    showAudioEndpointMenu(globalPos, false);
}

void RoomBottomBarWidget::showSpeakerDeviceMenu(const QPoint &globalPos) {
    showAudioEndpointMenu(globalPos, true);
}

void RoomBottomBarWidget::showAudioEndpointMenu(const QPoint &globalPos, bool speaker) {
    if (_inRecovery) return;
    auto *menu = createDeviceMenu(speaker ? "meetingSpeakerDeviceMenu" : "meetingMicrophoneDeviceMenu");
    const auto populate = [this, speaker](QMenu &target, const AudioDeviceDiscovery::Result *result) {
        target.clear();
        target.addAction(QCoreApplication::translate("MeetingUI", speaker ?
            "🔊 Select Speaker (Output)" : "🎤 Select Microphone (Input)"))->setEnabled(false);
        auto *group = new QActionGroup(&target);
        const auto add = [&](const QString &title, const QString &id) {
            auto *action = target.addAction(title);
            action->setData(id);
            action->setCheckable(true);
            action->setChecked(id == (speaker ? _currentSpeakerId : _currentMicId));
            group->addAction(action);
            connect(action, &QAction::triggered, this, [this, speaker, id] {
                if (_inRecovery) return;
                if (speaker) _speakerDeviceStream.fire_copy(id);
                else _micDeviceStream.fire_copy(id);
            });
        };
        add(QCoreApplication::translate("MeetingUI", "System Default"), QString());
        if (result && result->succeeded) {
            for (const auto &device : *result->devices) {
                auto title = QString::fromStdString(device.name);
                if (device.is_default) title += QCoreApplication::translate("MeetingUI", " (System Default)");
                add(title, QString::fromStdString(device.id));
            }
        } else {
            target.addAction(QCoreApplication::translate("MeetingUI", result ?
                "Failed to list audio devices" : "Loading audio devices..."))->setEnabled(false);
        }
        if (speaker) {
            target.addSeparator();
            auto *toggle = target.addAction(QCoreApplication::translate("MeetingUI", _speakerMuted ?
                "🔊 Enable Speaker Output" : "🔇 Mute Speaker Output"));
            connect(toggle, &QAction::triggered, this, &RoomBottomBarWidget::toggleSpeaker);
        }
    };
    populate(*menu, nullptr);
    auto &discovery = speaker ? (_speakerDiscovery ? *_speakerDiscovery : SpeakerDeviceDiscovery())
        : (_microphoneDiscovery ? *_microphoneDiscovery : MicrophoneDeviceDiscovery());
    loadDeviceMenu(menu, discovery, [populate](QMenu &target, const auto &result) { populate(target, &result); });
    menu->popup(globalPos);
}

void RoomBottomBarWidget::showVideoDeviceMenu(const QPoint &globalPos) {
    if (_inRecovery) return;
    auto *menu = createDeviceMenu("meetingCameraDeviceMenu");
    const auto populate = [this](QMenu &target, const CameraDeviceDiscovery::Result *result) {
        target.clear();
        target.addAction(QCoreApplication::translate("MeetingUI", "📷 Select Camera"))->setEnabled(false);
        if (result && result->succeeded) {
            const auto &devices = *result->devices;
            auto defaultDevice = std::find_if(devices.begin(), devices.end(),
                [](const auto &device) { return device.is_default; });
            if (defaultDevice == devices.end()) defaultDevice = devices.begin();
            auto *group = new QActionGroup(&target);
            for (const auto &device : devices) {
                const bool isDefault = &device == &*defaultDevice;
                const auto id = QString::fromStdString(device.path);
                auto title = QString::fromStdString(device.name);
                if (isDefault) title += QCoreApplication::translate("MeetingUI", " (System Default)");
                auto *action = target.addAction(title);
                action->setData(id);
                action->setCheckable(true);
                action->setChecked(_currentCameraPath.isEmpty() ? isDefault : _currentCameraPath == id);
                group->addAction(action);
                connect(action, &QAction::triggered, this, [this, id] {
                    if (_inRecovery) return;
                    _currentCameraPath = id;
                    _videoDeviceStream.fire_copy(id);
                });
            }
            if (devices.empty()) target.addAction(QCoreApplication::translate("MeetingUI", "No camera available"))->setEnabled(false);
        } else {
            target.addAction(QCoreApplication::translate("MeetingUI", result ?
                "Failed to list cameras" : "Loading cameras..."))->setEnabled(false);
        }
        target.addSeparator();
        auto *toggle = target.addAction(QCoreApplication::translate("MeetingUI", _videoEnabled ?
            "🚫 Stop Camera Video" : "📷 Start Camera Video"));
        connect(toggle, &QAction::triggered, this, &RoomBottomBarWidget::toggleVideo);
    };
    populate(*menu, nullptr);
    auto &discovery = _cameraDiscovery ? *_cameraDiscovery : CameraDeviceDiscovery::Instance();
    loadDeviceMenu(menu, discovery, [populate](QMenu &target, const auto &result) { populate(target, &result); });
    menu->popup(globalPos);
}

void RoomBottomBarWidget::mouseMoveEvent(QMouseEvent *e) {
	const QPoint pos = e->pos();
	int nextId = -1;

	if (!_inRecovery || canStopScreenShare()) {
		for (const auto &item : _toolItems) {
			if ((item.id == 3 ? screenShareControlEnabled() : !_inRecovery) && item.rect.contains(pos)) {
				nextId = item.id;
				break;
			}
		}
	}

	const bool nextEnd = _endMeetingRect.contains(pos);

	if (nextId != _hoveredId || nextEnd != _endHovered) {
		_hoveredId = nextId;
		_endHovered = nextEnd;
		update();
	}
}

void RoomBottomBarWidget::mousePressEvent(QMouseEvent *e) {
	if (_inRecovery) {
		// Capture can always be stopped, including while transport recovery
		// disables other operations. Network unpublish converges after recovery.
		if (e->button() == Qt::LeftButton && canStopScreenShare()) {
			for (const auto &item : _toolItems) {
				if (item.id == 3 && item.rect.contains(e->pos())) {
					_shareScreenStream.fire({});
					return;
				}
			}
		}
		if (e->button() == Qt::LeftButton && _endMeetingRect.contains(e->pos())) {
			_endMeetingStream.fire({});
		}
		return;
	}

	if (e->button() == Qt::RightButton) {
		for (const auto &item : _toolItems) {
			if (item.rect.contains(e->pos())) {
				if (item.id == 1) {
					showAudioDeviceMenu(mapToGlobal(QPoint(item.rect.left(), item.rect.top() - 10)));
					return;
				} else if (item.id == 11) {
					showSpeakerDeviceMenu(mapToGlobal(QPoint(item.rect.left(), item.rect.top() - 10)));
					return;
				} else if (item.id == 2) {
					showVideoDeviceMenu(mapToGlobal(QPoint(item.rect.left(), item.rect.top() - 10)));
					return;
				}
			}
		}
	}

	if (e->button() == Qt::LeftButton) {
		if (_endMeetingRect.contains(e->pos())) {
			_endMeetingStream.fire({});
			return;
		}

		for (const auto &item : _toolItems) {
			if (item.rect.contains(e->pos())) {
				switch (item.id) {
				case 1: {
					// 如果点击的是右侧下拉小三角区域 (宽 16px)
					if (e->pos().x() > item.rect.right() - 16) {
						showAudioDeviceMenu(mapToGlobal(QPoint(item.rect.left(), item.rect.top() - 10)));
						break;
					}
					toggleAudio();
					break;
				}
				case 11: {
					// 如果点击的是右侧下拉小三角区域 (宽 16px)
					if (e->pos().x() > item.rect.right() - 16) {
						showSpeakerDeviceMenu(mapToGlobal(QPoint(item.rect.left(), item.rect.top() - 10)));
						break;
					}
					toggleSpeaker();
					break;
				}
				case 2: {
					// 如果点击的是右侧下拉小三角区域 (宽 16px)
					if (e->pos().x() > item.rect.right() - 16) {
						showVideoDeviceMenu(mapToGlobal(QPoint(item.rect.left(), item.rect.top() - 10)));
						break;
					}
					toggleVideo();
					break;
				}
				case 3:
					if (screenShareControlEnabled()) _shareScreenStream.fire({});
					break;
				case 4:
					_inviteStream.fire({});
					break;
				case 5:
					_participantsStream.fire({});
					break;
				case 6:
					_chatStream.fire({});
					break;
				case 7:
					_whiteboardStream.fire({});
					break;
				case 10:
					showSimulateScenarioMenu(mapToGlobal(QPoint(item.rect.left(), item.rect.top() - 10)));
					break;
				}
				break;
			}
		}
	}
}

void RoomBottomBarWidget::showSimulateScenarioMenu(const QPoint &globalPos) {
	QMenu menu(this);
	AppTheme::styleMenu(menu, AppTheme::Tone::Dark);

	QAction *header = menu.addAction(QCoreApplication::translate("MeetingUI", "Simulate Scenario"));
	header->setEnabled(false);
	menu.addSeparator();

	struct ScenarioEntry {
		QString name;
		livekit::SimulateScenarioType type;
	};

	const std::vector<ScenarioEntry> entries = {
		{ QCoreApplication::translate("MeetingUI", "Signal reconnect"), livekit::SimulateScenarioType::SignalReconnect },
		{ QCoreApplication::translate("MeetingUI", "Full reconnect"), livekit::SimulateScenarioType::FullReconnect },
		{ QCoreApplication::translate("MeetingUI", "Speaker update"), livekit::SimulateScenarioType::SpeakerUpdate },
		{ QCoreApplication::translate("MeetingUI", "Node failure"), livekit::SimulateScenarioType::NodeFailure },
		{ QCoreApplication::translate("MeetingUI", "Server migration"), livekit::SimulateScenarioType::Migration },
		{ QCoreApplication::translate("MeetingUI", "Server disconnect"), livekit::SimulateScenarioType::ServerLeave },
		{ QCoreApplication::translate("MeetingUI", "Switch ICE candidate"), livekit::SimulateScenarioType::SwitchCandidate },
		{ QCoreApplication::translate("MeetingUI", "Rotate E2EE key"), livekit::SimulateScenarioType::E2eeKeyRatchet },
		{ QCoreApplication::translate("MeetingUI", "Update participant name"), livekit::SimulateScenarioType::ParticipantName },
		{ QCoreApplication::translate("MeetingUI", "Update participant metadata"), livekit::SimulateScenarioType::ParticipantMetadata },
		{ QCoreApplication::translate("MeetingUI", "Clear simulation"), livekit::SimulateScenarioType::Clear }
	};

	for (const auto &entry : entries) {
		QAction *act = menu.addAction(entry.name);
		connect(act, &QAction::triggered, [this, type = entry.type] {
			_simulateScenarioStream.fire_copy(type);
		});
	}

	menu.exec(globalPos);
}

void RoomBottomBarWidget::leaveEventHook(QEvent *e) {
	_hoveredId = -1;
	_endHovered = false;
	update();
	Ui::RpWidget::leaveEventHook(e);
}

// ----------------------------------------------------
// MeetingRoomWindow 实现
// ----------------------------------------------------

// Only the native worker touches these resources until Qt accepts completion.
// Cancellation is cooperative: an in-flight driver call finishes on its owner
// thread, then queued cleanup releases everything without blocking the window.
struct MeetingRoomWindow::MediaPreparation {
	std::atomic_bool cancelled{false};
	std::shared_ptr<livekit::WasapiAudioCapture> microphone;
	std::shared_ptr<livekit::CameraSourceManager> camera;
	bool cameraAvailable = false;
	QString cameraPath;
};

MeetingRoomWindow::MeetingRoomWindow(const Config &config,
                                     std::shared_ptr<OpenMeeting::MeetingCoordinator> coordinator,
                                     QWidget *parent)
	: Ui::RpWidget(parent)
	, _config(config)
	, _coordinator(std::move(coordinator)) {
	setObjectName("MeetingRoomWindow");
	AppTheme::setTone(*this, AppTheme::Tone::Dark);
	if (!_coordinator) {
		_coordinator = OpenMeeting::MeetingCoordinator::create(this);
	}
	setupCameraCompletionOwner(OpenMeeting::SessionManager::instance());
	setWindowTitle(QCoreApplication::translate("MeetingUI", "Cohavora Meeting Room - %1").arg(config.displayName));
	resize(1120, 720);
	setMinimumSize(850, 560);
	if (!parent) {
		AppTheme::centerOnScreen(*this);
	}
	setMouseTracking(true);

	// Keep Qt's native frame bookkeeping consistent with the resizable HWND.
	// WM_NCCALCSIZE removes the native title bar while retaining its resize frame.
	setWindowFlags(Qt::Window | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint);

	_preparingMedia = true;
	initLayout();
	setupCoordinatorBindings();
	_coordinator->setLocalAudioMuted(_config.audioMuted);
	_coordinator->setLocalVideoEnabled(_config.videoEnabled);
	_remoteRenderSession = std::make_unique<livekit::render::VideoRenderSession>(
		[this](const std::string &identity, const QImage &image,
		       livekit::render::VideoRenderFrame::Ptr frame) {
			receiveRenderedVideoFrame(
				image, QString::fromStdString(identity), std::move(frame));
		});
	_remoteRenderTimer = new QTimer(this);
	connect(_remoteRenderTimer, &QTimer::timeout, this, &MeetingRoomWindow::onRemoteRenderTick);
	_remoteRenderTimer->start(33);

	// 3. 复用 Coordinator 的本地音频与视频数据源，确保外设采集与 WebRTC 发送通道打通
	bindLocalMediaSources();

	const auto captureUiCallbacks = _captureUiCallbacks;
	_localAudioSource->addSink([captureUiCallbacks](const livekit::AudioFrame &frame) {
		if (!captureUiCallbacks->active()) return;
		const auto &samples = frame.data();
		if (!samples.empty()) {
			double sum = 0.0;
			for (size_t i = 0; i < samples.size(); ++i) {
				sum += static_cast<double>(samples[i]) * samples[i];
			}
			double rms = std::sqrt(sum / static_cast<double>(samples.size()));
			float level = static_cast<float>(std::clamp(rms / 2200.0, 0.0, 1.0));
			bool speaking = (level > 0.02f);
			captureUiCallbacks->Post([speaking, level](MeetingRoomWindow *window) {
				if (window->_sessionRunning.load(std::memory_order_acquire) &&
					window->_localTile && !window->_config.audioMuted) {
					window->_localTile->setSpeaking(speaking, level);
				}
			});
		}
	});


	_localTile->setVideoActive(false);
	_localTile->setAudioMuted(_config.audioMuted);
	updateVideoLayout();

	attachCoordinatorSession();
	scheduleViewportIntent(true);

	updateRecoveryStateUi(_pendingAdmission ? OpenMeeting::MeetingState::Idle :
		(_coordinator ? _coordinator->state() : OpenMeeting::MeetingState::InMeeting), {});
}

void MeetingRoomWindow::prepareMediaAndJoin(std::function<void()> admission) {
	Q_ASSERT(thread() == QThread::currentThread());
	if (_mediaPreparationStarted || !_sessionRunning || _closeRequested ||
		_closingForSessionInvalidation || !admission) return;
	_mediaPreparationStarted = true;
	_pendingAdmission = std::move(admission);
	beginMediaPreparation(!_config.audioMuted, _config.videoEnabled);
}

void MeetingRoomWindow::beginMediaPreparation(bool microphone, bool camera) {
	Q_ASSERT(thread() == QThread::currentThread());
	if (_mediaPreparation || !_sessionRunning || _closeRequested ||
		_closingForSessionInvalidation) return;
	_preparingMedia = true;
	_mediaPreparation = std::make_shared<MediaPreparation>();
	_preparationCallbacks = OpenMeeting::QtCallbackGate<MeetingRoomWindow>::Create(this);
	updateRecoveryStateUi(OpenMeeting::MeetingState::Idle, {});
	const auto preparation = _mediaPreparation;
	const auto callbacks = _preparationCallbacks;
	const auto preferences = _cameraSessionManager
		? _cameraSessionManager->mediaPreferences()
		: OpenMeeting::SessionManager::instance().mediaPreferences();
	const auto audioSource = _localAudioSource;
	const auto videoSource = _localVideoSource;
	const bool prepareMicrophone = microphone && !_wasapiCap;
	const bool prepareCamera = camera && !_cameraManager;
	const auto testPreparation = _mediaPreparationForTest;
	// This application-owned MTA queue also participates in shutdown draining.
	// WASAPI initializes on its capture thread; DirectShow uses its graph owner.
	// No job reads a QWidget, QPointer or SessionManager.
	const bool accepted = OpenMeeting::SessionShutdownService::Instance().Submit(
		[preparation, callbacks, preferences, audioSource, videoSource,
		 prepareMicrophone, prepareCamera, testPreparation] {
			if (preparation->cancelled.load()) return;
			if (prepareMicrophone) {
				if (testPreparation) testPreparation(true, false);
				else try {
					auto microphone = livekit::WasapiAudioCapture::Create();
					preparation->microphone = microphone;
					microphone->EnableApm();
					const auto processor = microphone->apm_processor();
					if (processor) {
						auto config = processor->GetConfig();
						config.enable_aec = preferences.echoCancellation;
						config.enable_ans = preferences.noiseSuppression;
						config.enable_agc = preferences.autoGainControl;
						processor->ApplyConfig(config);
					}
					// Set intent before Start: muted entry must not briefly capture unmuted.
					// Keep preparation silent until the Qt owner commits the request.
					microphone->SetMute(true);
					livekit::WasapiCaptureConfig config;
					config.type = livekit::WasapiCaptureType::Microphone;
					config.device_id = preferences.microphoneDeviceId.toStdString();
					config.target_sample_rate = 48000;
					config.target_channels = 2;
					if (!preparation->cancelled.load() && microphone->Init(config, audioSource)) {
						if (!preparation->cancelled.load()) microphone->Start();
					}
				} catch (...) {
					// Device absence/failure degrades that medium, not admission.
				}
			}
			if (preparation->cancelled.load()) return;
			if (prepareCamera) {
				if (testPreparation) testPreparation(false, true);
				else try {
					// One snapshot supplies default selection, saved selection and capabilities.
					const auto devices = livekit::DShowEnumerator::EnumerateVideoDevices();
					if (preparation->cancelled.load()) return;
					auto selected = std::find_if(devices.begin(), devices.end(),
						[](const livekit::DShowDeviceInfo &device) { return device.is_default; });
					if (selected == devices.end()) selected = devices.begin();
					if (!preferences.cameraDeviceId.isEmpty()) {
						const auto id = preferences.cameraDeviceId.toStdString();
						const auto saved = std::find_if(devices.begin(), devices.end(),
							[&](const livekit::DShowDeviceInfo &device) {
								return device.path == id || device.name == id;
							});
						if (saved != devices.end()) selected = saved;
					}
					if (selected != devices.end() && !selected->path.empty()) {
						const auto resolutions = livekit::CameraSourceManager::GetSupportedResolutions(*selected);
						const auto requested = std::find_if(resolutions.begin(), resolutions.end(),
							[&](const livekit::CameraResolution &resolution) {
								return resolution.width == preferences.videoCaptureWidth &&
									resolution.height == preferences.videoCaptureHeight;
							});
						const auto resolution = requested != resolutions.end()
							? std::optional<livekit::CameraResolution>(*requested)
							: livekit::CameraSourceManager::SelectDefaultResolution(resolutions);
						livekit::DShowCaptureConfig config;
						config.device_path = selected->path;
						config.output_format = livekit::VideoBufferType::NV12;
						if (resolution) {
							config.width = resolution->width;
							config.height = resolution->height;
							const int fps = requested != resolutions.end() ? preferences.videoCaptureFps : 30;
							config.fps = resolution->max_fps > 0 ? (std::min)(fps, resolution->max_fps) : fps;
						}
						if (preparation->cancelled.load()) return;
						preparation->camera = livekit::CameraSourceManager::Create(videoSource);
						preparation->cameraAvailable = preparation->camera->Start(config);
						if (preparation->cameraAvailable) preparation->cameraPath = QString::fromStdString(selected->path);
					}
				} catch (...) {
					preparation->cameraAvailable = false;
				}
			}
		}, [callbacks](std::exception_ptr failure) {
			callbacks->Post([failure](MeetingRoomWindow *window) {
				if (failure) {
					window->cancelMediaPreparation();
					window->updateRecoveryStateUi(OpenMeeting::MeetingState::Failed,
						QCoreApplication::translate("MeetingUI", "Unable to prepare audio and video devices."));
					return;
				}
				window->finishMediaPreparation();
			});
		});
	if (!accepted) {
		// No worker ran and no native resources exist when admission is rejected.
		_mediaPreparation.reset();
		cancelMediaPreparation();
		updateRecoveryStateUi(OpenMeeting::MeetingState::Failed,
			QCoreApplication::translate("MeetingUI", "Unable to prepare audio and video devices."));
	}
}

void MeetingRoomWindow::finishMediaPreparation() {
	Q_ASSERT(thread() == QThread::currentThread());
	if (!_mediaPreparation || _mediaPreparation->cancelled.load() ||
		!_sessionRunning || _closeRequested || _closingForSessionInvalidation) return;
	const bool newMicrophone = bool(_mediaPreparation->microphone);
	if (newMicrophone) _wasapiCap = std::move(_mediaPreparation->microphone);
	if (_mediaPreparation->camera) {
		_cameraManager = std::move(_mediaPreparation->camera);
		_usingRealCamera = _mediaPreparation->cameraAvailable;
		_currentCameraPath = _mediaPreparation->cameraPath;
	}
	if (!_cameraManager) _usingRealCamera = false;
	_mediaPreparation.reset();
	_preparingMedia = false;
	if (newMicrophone) {
		bindMicrophoneCaptureState();
		livekit::WebRTCManager::Instance().SetApmProcessor(_wasapiCap->apm_processor());
	}
	if (_cameraSessionManager) setupAudioPreferencesBinding(*_cameraSessionManager);
	// Availability projection may synchronously emit the previous mute state.
	// Retain the user's current intent before changing coordinator availability.
	const bool audioMuted = _config.audioMuted || !_wasapiCap || !_wasapiCap->IsRunning();
	const bool videoEnabled = _config.videoEnabled && _usingRealCamera;
	applyMicrophoneAvailability(_wasapiCap && _wasapiCap->IsRunning());
	_config.audioMuted = audioMuted;
	_config.videoEnabled = videoEnabled;
	if (_coordinator) {
		_coordinator->setLocalVideoAvailable(_usingRealCamera);
		_coordinator->setLocalAudioMuted(audioMuted);
		_coordinator->setLocalVideoEnabled(videoEnabled);
	}
	if (_wasapiCap) _wasapiCap->SetMute(audioMuted);
	if (_bottomBar) {
		_bottomBar->setAudioMuted(_config.audioMuted);
		_bottomBar->setVideoEnabled(_config.videoEnabled);
	}
	if (_localTile) {
		_localTile->setVideoActive(_config.videoEnabled && _usingRealCamera);
		_localTile->setAudioMuted(_config.audioMuted);
	}
	LogToConsole(LogCategory::Media, "CAPTURE",
		QStringLiteral("devices_prepared microphone=%1 camera=%2").arg(_microphoneAvailable).arg(_usingRealCamera));
	updateRecoveryStateUi(_pendingAdmission ? OpenMeeting::MeetingState::ConnectingRoom :
		(_coordinator ? _coordinator->state() : OpenMeeting::MeetingState::InMeeting), {});
	auto admission = std::move(_pendingAdmission);
	if (admission) admission();
}

void MeetingRoomWindow::requestLocalMicrophone(bool muted) {
	if (!_sessionRunning || _closeRequested || _closingForSessionInvalidation) return;
	_config.audioMuted = muted;
	if (!muted && !_wasapiCap) {
		if (!_mediaPreparation) beginMediaPreparation(true, false);
		return;
	}
	if (_coordinator) _coordinator->setLocalAudioMuted(muted);
}

void MeetingRoomWindow::requestLocalCamera(bool enabled) {
	if (!_sessionRunning || _closeRequested || _closingForSessionInvalidation) return;
	_config.videoEnabled = enabled;
	if (enabled && !_cameraManager) {
		if (!_mediaPreparation) beginMediaPreparation(false, true);
		return;
	}
	if (_coordinator) _coordinator->setLocalVideoEnabled(enabled);
}

void MeetingRoomWindow::cancelMediaPreparation() {
	Q_ASSERT(thread() == QThread::currentThread());
	_pendingAdmission = {};
	_preparingMedia = false;
	if (_preparationCallbacks) _preparationCallbacks->Revoke();
	if (auto preparation = std::move(_mediaPreparation)) {
		preparation->cancelled.store(true);
		// FIFO with preparation: never destroy a device while Start is using it.
		OpenMeeting::SessionShutdownService::Instance().SubmitCleanup(
			[preparation = std::move(preparation)]() mutable {
				if (preparation->microphone) preparation->microphone->Stop();
				if (preparation->camera) preparation->camera->Stop();
				preparation.reset();
			});
	}
}

MeetingRoomWindow::MeetingRoomWindow(
		ParticipantWindowTestTag,
		const Config &config,
		std::shared_ptr<OpenMeeting::MeetingCoordinator> coordinator,
		bool fullLayoutForChatPrivacy,
		QWidget *parent)
:	Ui::RpWidget(parent),
	_coordinator(std::move(coordinator)),
	_config(config) {
	// Isolated presentation fixture: the bindings, tile operations and CPU
	// renderer are production paths; no device capture or account singleton.
	setObjectName("MeetingRoomWindowParticipantFixture");
	AppTheme::setTone(*this, AppTheme::Tone::Dark);
	if (fullLayoutForChatPrivacy) {
		initLayout();
	} else {
		_topBar = new RoomTopBarWidget(this);
		_stageContainer = new QWidget(this);
		_bottomBar = new RoomBottomBarWidget(this);
		_localTile = new VideoTileWidget(_config.displayName, true, _stageContainer);
		_localTile->setIdentity("local");
		bindTileInteractions(_localTile);
		_localTile->setAudioMuted(true);
		_localTile->setVideoActive(false);
		_inviteHintBanner = new QLabel(_stageContainer);
		_recoveryBanner = new QLabel(_stageContainer);
		_recoveryBanner->hide();
		setupInvitationBinding();
		setupWhiteboardBinding();
		setupVideoPagingControls();
	}
	_remoteRenderSession = std::make_unique<livekit::render::VideoRenderSession>(
		[this](const std::string &identity, const QImage &image,
		       livekit::render::VideoRenderFrame::Ptr frame) {
			receiveRenderedVideoFrame(
				image, QString::fromStdString(identity), std::move(frame));
		});
	_remoteRenderSession->UseQtCpuBackend();
	bindLocalMediaSources();
	setupCoordinatorBindings();
	resize(1120, 720);
	_stageContainer->setGeometry(0, 56, 1120, 588);
	attachCoordinatorSession();
	scheduleViewportIntent(true);
}

MeetingRoomWindow::MeetingRoomWindow(
		CameraOwnerTestTag,
		const Config &config,
		std::shared_ptr<livekit::CameraSourceManager> cameraManager,
		OpenMeeting::SessionManager &sessionManager,
		QWidget *parent)
:	Ui::RpWidget(parent),
	_config(config),
	_cameraManager(std::move(cameraManager)) {
	setObjectName("MeetingRoomWindowCameraOwnerFixture");
	AppTheme::setTone(*this, AppTheme::Tone::Dark);
	_topBar = new RoomTopBarWidget(this);
	_stageContainer = new QWidget(this);
	_bottomBar = new RoomBottomBarWidget(this);
	_localTile = new VideoTileWidget(
		QCoreApplication::translate("MeetingUI", "%1 (Me)").arg(_config.displayName),
		true,
		_stageContainer);
	_localTile->setIdentity("local");
	_localTile->setVideoActive(_config.videoEnabled);
	setupCameraCompletionOwner(sessionManager);
	bindCameraDeviceChanges();
}

MeetingRoomWindow::~MeetingRoomWindow() {
    _remoteControlUi.reset();
	if (_nativeResizeFilterInstalled && qApp) qApp->removeNativeEventFilter(this);
	if (_departureNotice) {
		// The notice is intentionally not parented to the meeting window.  Close
		// it explicitly when another teardown path destroys the window first.
		QObject::disconnect(_departureNotice, nullptr, this, nullptr);
		_departureNotice->close();
		_departureNotice.clear();
	}
	invalidateCameraCompletion();
	stopLiveKitSession();
}

void MeetingRoomWindow::showEvent(QShowEvent *e) {
	Ui::RpWidget::showEvent(e);
	_logicalWindowVisible = true;
	setupNativeWindow();
	tryActivateGpuBackend();
	scheduleViewportIntent(true);
}

void MeetingRoomWindow::hideEvent(QHideEvent *e) {
	_logicalWindowVisible = false;
	scheduleViewportIntent(true);
	updateVideoLayout();
	Ui::RpWidget::hideEvent(e);
}

void MeetingRoomWindow::changeEvent(QEvent *e) {
	Ui::RpWidget::changeEvent(e);
	if (e && e->type() == QEvent::WindowStateChange) {
		scheduleViewportIntent(true);
		updateVideoLayout();
	}
}

void MeetingRoomWindow::closeEvent(QCloseEvent *e) {
	if (!_closeRequested) {
		_closeRequested = true;
		const bool requestLeave = !_closingForSessionInvalidation &&
			!OpenMeeting::SessionManager::instance().isSessionInvalidating();
		stopLiveKitSession(requestLeave);
	}
	Ui::RpWidget::closeEvent(e);
}

void MeetingRoomWindow::setupNativeWindow() {
#if defined(Q_OS_WIN)
	_handle = reinterpret_cast<HWND>(winId());
	if (!_handle) return;
	if (!_nativeResizeFilterInstalled) {
		qApp->installNativeEventFilter(this);
		_nativeResizeFilterInstalled = true;
	}

	LONG_PTR style = GetWindowLongPtr(_handle, GWL_STYLE);
	// Windows 10 can paint a native caption even with our thin NCCALCSIZE
	// inset. Keep the resize frame, but let only the Qt top bar own the caption.
	SetWindowLongPtr(_handle, GWL_STYLE,
		(style & ~LONG_PTR(WS_CAPTION)) | WS_THICKFRAME | WS_SYSMENU |
		WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN);

	MARGINS margins = { 1, 1, 1, 1 };
	DwmExtendFrameIntoClientArea(_handle, &margins);

	DWORD preference = 2; // DWMWCP_ROUND
	DwmSetWindowAttribute(_handle, 33, &preference, sizeof(preference));

	SetWindowPos(_handle, nullptr, 0, 0, 0, 0,
		SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
}

void MeetingRoomWindow::initLayout() {
	_topBar = new RoomTopBarWidget(this);
	connect(_topBar, &RoomTopBarWidget::windowDragRequested, this, [this]() {
		if (isFullScreen()) {
			return;
		}
		if (auto *handle = windowHandle()) {
			handle->startSystemMove();
		}
	});
	QString mId = _coordinator ? _coordinator->currentMeetingId() : QString();
	if (mId.isEmpty()) mId = _config.meetingId;
	if (!mId.isEmpty()) {
		_topBar->setMeetingId(mId);
		setWindowTitle(QCoreApplication::translate("MeetingUI", "Cohavora - Meeting ID: %1").arg(mId));
	}
	_stageContainer = new QWidget(this);
	MeetingUI::AppTheme::setStyleVariant(*_stageContainer, "meeting-room-window-stagecontainer");
	if ((_videoCanvas = livekit::render::CreateVideoCanvas(_stageContainer, &_renderDiagnostics))) {
		_videoCanvas->setGeometry(_stageContainer->rect());
		_videoCanvas->hide();
		connect(_videoCanvas, &livekit::render::VideoCanvas::rendererUnavailable,
		        this, &MeetingRoomWindow::fallBackToQtCpuBackend, Qt::QueuedConnection);
	} else {
		LogToConsole(LogCategory::WebRTC, "RENDER",
			QCoreApplication::translate("MeetingUI", "Qt CPU video backend: ") + livekit::render::RenderDiagnosticsSafeSummary(_renderDiagnostics));
	}
	_bottomBar = new RoomBottomBarWidget(this);

	_participantsSidebar = new OpenMeeting::ParticipantsSidebarWidget(_coordinator, this);
	_participantsSidebar->hide();
	connect(_participantsSidebar, &OpenMeeting::ParticipantsSidebarWidget::closeRequested, this, [this]() {
		switchSidebar(ActiveSidebar::None);
	});

	_chatSidebar = new OpenMeeting::MeetingChatSidebarWidget(this);
	_chatSidebar->hide();
	connect(_chatSidebar, &OpenMeeting::MeetingChatSidebarWidget::closeRequested, this, [this]() {
		switchSidebar(ActiveSidebar::None);
	});
	connect(_chatSidebar, &OpenMeeting::MeetingChatSidebarWidget::messageSent, this, [this](const QString &text) {
		QString msgId = QString("msg_%1_%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(qrand() % 1000);
		int64_t seq = _coordinator ? _coordinator->nextSequenceNumber() : (QDateTime::currentMSecsSinceEpoch() * 1000);
		OpenMeeting::ChatMessageItem item;
		item.id = msgId;
		item.senderIdentity = "local";
		item.senderName = QCoreApplication::translate("MeetingUI", "%1 (Me)").arg(_config.displayName);
		item.text = text;
		item.timestamp = QDateTime::currentMSecsSinceEpoch();
		item.seq = seq;
		item.isMine = true;
		item.status = OpenMeeting::MessageSendStatus::Sending;
		_chatSidebar->appendMessage(item);

		if (_coordinator) {
			_coordinator->sendChatMessage(text, msgId, seq);
		}
		LogToConsole(LogCategory::Participant, "CHAT_TEXT_SENT",
			QStringLiteral("bytes=%1").arg(text.toUtf8().size()));
	});

	connect(_chatSidebar, &OpenMeeting::MeetingChatSidebarWidget::imageSent, this, [this](const QString &fileName, const QByteArray &data) {
		QString msgId = QString("img_%1_%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(qrand() % 1000);
		int64_t seq = _coordinator ? _coordinator->nextSequenceNumber() : (QDateTime::currentMSecsSinceEpoch() * 1000);
		OpenMeeting::ChatMessageItem item;
		item.id = msgId;
		item.senderIdentity = "local";
		item.senderName = QCoreApplication::translate("MeetingUI", "%1 (Me)").arg(_config.displayName);
		item.type = OpenMeeting::ChatMessageType::Image;
		item.fileName = fileName;
		item.fileSize = data.size();
		item.fileData = data;
		item.timestamp = QDateTime::currentMSecsSinceEpoch();
		item.seq = seq;
		item.isMine = true;
		item.status = OpenMeeting::MessageSendStatus::Sending;
		item.progress = 0;
		_chatSidebar->appendMessage(item);

		if (_coordinator) {
			_coordinator->sendChatMediaMessage(msgId, "image", fileName, data, seq);
		}
		LogToConsole(LogCategory::Participant, "CHAT_IMAGE_SENT",
			QStringLiteral("bytes=%1").arg(data.size()));
	});

	connect(_chatSidebar, &OpenMeeting::MeetingChatSidebarWidget::fileSent, this, [this](const QString &fileName, const QByteArray &data) {
		QString msgId = QString("file_%1_%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(qrand() % 1000);
		int64_t seq = _coordinator ? _coordinator->nextSequenceNumber() : (QDateTime::currentMSecsSinceEpoch() * 1000);
		OpenMeeting::ChatMessageItem item;
		item.id = msgId;
		item.senderIdentity = "local";
		item.senderName = QCoreApplication::translate("MeetingUI", "%1 (Me)").arg(_config.displayName);
		item.type = OpenMeeting::ChatMessageType::File;
		item.fileName = fileName;
		item.fileSize = data.size();
		item.fileData = data;
		item.timestamp = QDateTime::currentMSecsSinceEpoch();
		item.seq = seq;
		item.isMine = true;
		item.status = OpenMeeting::MessageSendStatus::Sending;
		item.progress = 0;
		_chatSidebar->appendMessage(item);

		if (_coordinator) {
			_coordinator->sendChatMediaMessage(msgId, "file", fileName, data, seq);
		}
		LogToConsole(LogCategory::Participant, "CHAT_FILE_SENT",
			QStringLiteral("bytes=%1").arg(data.size()));
	});

	connect(_chatSidebar, &OpenMeeting::MeetingChatSidebarWidget::retryRequested, this, [this](const QString &msgId) {
		if (!_coordinator || !_chatSidebar) return;
		auto msg = _chatSidebar->findMessage(msgId);
		if (msg.id.isEmpty()) return;

		_chatSidebar->updateMessageStatus(msgId, OpenMeeting::MessageSendStatus::Sending, 0);

		if (msg.type == OpenMeeting::ChatMessageType::Text) {
			_coordinator->sendChatMessage(msg.text, msgId, msg.seq);
		} else if (msg.type == OpenMeeting::ChatMessageType::Image) {
			_coordinator->sendChatMediaMessage(msgId, "image", msg.fileName, msg.fileData, msg.seq);
		} else if (msg.type == OpenMeeting::ChatMessageType::File) {
			_coordinator->sendChatMediaMessage(msgId, "file", msg.fileName, msg.fileData, msg.seq);
		}
	});

	_localTile = new VideoTileWidget(QCoreApplication::translate("MeetingUI", "%1 (Me)").arg(_config.displayName), true, _stageContainer);
	_localTile->setIdentity("local");
	_localTile->show();

	bindTileInteractions(_localTile);

	_bottomBar->setAudioMuted(_config.audioMuted);
	_bottomBar->setVideoEnabled(_config.videoEnabled);
	_bottomBar->setParticipantCount(1);

	_localTile->setAudioMuted(_config.audioMuted);
	_localTile->setVideoActive(_config.videoEnabled);

	_inviteHintBanner = new QLabel(QCoreApplication::translate("MeetingUI", "Waiting for more participants..."), _stageContainer);
	_inviteHintBanner->setAlignment(Qt::AlignCenter);
	MeetingUI::AppTheme::setStyleVariant(*_inviteHintBanner, "meeting-room-window-invitehintbanner");

	_recoveryBanner = new QLabel(_stageContainer);
	_recoveryBanner->setAlignment(Qt::AlignCenter);
	_recoveryBanner->hide();
	setupVideoPagingControls();

	_recoveryBannerFadeTimer = new QTimer(this);
	_recoveryBannerFadeTimer->setSingleShot(true);
	connect(_recoveryBannerFadeTimer, &QTimer::timeout, this, [this]() {
		if (_recoveryBanner) {
			_recoveryBanner->hide();
		}
	});

	_meetingTimer = new QTimer(this);
	connect(_meetingTimer, &QTimer::timeout, this, &MeetingRoomWindow::onTimerTick);
	_meetingTimer->start(1000);

	_topBar->minimizeClicked() | rpl::on_next([this] { showMinimized(); }, lifetime());
	_topBar->maximizeClicked() | rpl::on_next([this] {
		if (isMaximized()) showNormal();
		else showMaximized();
	}, lifetime());
	_topBar->closeClicked() | rpl::on_next([this] { close(); }, lifetime());
	_topBar->viewModeChanged() | rpl::on_next([this](VideoViewMode m) {
		_viewMode = m;
		_videoPage = 0;
		scheduleViewportIntent(true);
	}, lifetime());
	_topBar->consoleClicked() | rpl::on_next([this] {
		MeetingLogConsoleWindow::Instance().show();
		MeetingLogConsoleWindow::Instance().raise();
		MeetingLogConsoleWindow::Instance().activateWindow();
	}, lifetime());

	auto handleSimulate = [this](livekit::SimulateScenarioType type) {
        if (type == livekit::SimulateScenarioType::E2eeKeyRatchet && _room) {
            const auto manager = _room->e2ee_manager();
            if (manager && manager->enabled() && manager->encryption_type() != livekit::EncryptionType::NONE) {
                LogToConsole(LogCategory::General, "E2EE",
                    QCoreApplication::translate("MeetingUI", "Debug key rotation is unavailable in an encrypted meeting."));
                return;
            }
        }
		if (_room) {
			_room->SimulateScenario(type);
		}
		QString name;
		switch (type) {
		case livekit::SimulateScenarioType::SignalReconnect: name = "signalReconnect"; break;
		case livekit::SimulateScenarioType::FullReconnect: name = "fullReconnect"; break;
		case livekit::SimulateScenarioType::SpeakerUpdate: name = "speakerUpdate"; break;
		case livekit::SimulateScenarioType::NodeFailure: name = "nodeFailure"; break;
		case livekit::SimulateScenarioType::Migration: name = "migration"; break;
		case livekit::SimulateScenarioType::ServerLeave: name = "serverLeave"; break;
		case livekit::SimulateScenarioType::SwitchCandidate: name = "switchCandidate"; break;
		case livekit::SimulateScenarioType::E2eeKeyRatchet: name = "e2eeKeyRatchet"; break;
		case livekit::SimulateScenarioType::ParticipantName: name = "participantName"; break;
		case livekit::SimulateScenarioType::ParticipantMetadata: name = "participantMetadata"; break;
		case livekit::SimulateScenarioType::Clear: name = "clear"; break;
		}
		LogToConsole(LogCategory::General, "SIMULATE", QCoreApplication::translate("MeetingUI", "Scenario simulation triggered: %1").arg(name));
	};

	_topBar->simulateScenarioRequested() | rpl::on_next(handleSimulate, lifetime());
	_bottomBar->simulateScenarioRequested() | rpl::on_next(handleSimulate, lifetime());

	_bottomBar->toggleAudioRequested() | rpl::on_next([this](bool muted) {
		requestLocalMicrophone(muted);
		LogToConsole(LogCategory::Media, "AUDIO", _config.audioMuted ? QCoreApplication::translate("MeetingUI", "User muted the microphone") : QCoreApplication::translate("MeetingUI", "User enabled or unmuted the microphone"));
	}, lifetime());

	_bottomBar->toggleSpeakerRequested() | rpl::on_next([this](bool muted) {
		setSpeakerOutputMuted(muted);
		LogToConsole(LogCategory::Media, "SPEAKER", _bottomBar->isSpeakerMuted() ? QCoreApplication::translate("MeetingUI", "User muted speaker output") : QCoreApplication::translate("MeetingUI", "User enabled speaker output"));
	}, lifetime());

	_bottomBar->toggleVideoRequested() | rpl::on_next([this](bool enabled) {
		requestLocalCamera(enabled);
		LogToConsole(LogCategory::Media, "VIDEO", _config.videoEnabled ? QCoreApplication::translate("MeetingUI", "User enabled local video") : QCoreApplication::translate("MeetingUI", "User disabled local video"));
	}, lifetime());

	setupInvitationBinding();

	setupWhiteboardBinding();

	_bottomBar->participantsClicked() | rpl::on_next([this] {
		switchSidebar(ActiveSidebar::Participants);
	}, lifetime());

	_bottomBar->chatClicked() | rpl::on_next([this] {
		switchSidebar(ActiveSidebar::Chat);
	}, lifetime());

	if (_coordinator) {
		connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMessageReceived,
			this, [this](const QString &senderIdentity, const QString &senderName, const QString &text, int64_t seq) {
			const auto dispName = resolveParticipantDisplayName(
				_coordinator, senderIdentity, senderName);

			OpenMeeting::ChatMessageItem item;
			item.id = QString::number(QDateTime::currentMSecsSinceEpoch());
			item.senderIdentity = senderIdentity;
			item.senderName = dispName;
			item.text = text;
			item.timestamp = QDateTime::currentMSecsSinceEpoch();
			item.seq = seq;
			item.isMine = false;

			if (_chatSidebar) {
				_chatSidebar->appendMessage(item);
			}

			if (_activeSidebar != ActiveSidebar::Chat && _bottomBar) {
				_bottomBar->setChatUnreadCount(_bottomBar->chatUnreadCount() + 1);
			}

			LogToConsole(LogCategory::Participant, "CHAT_TEXT_RECEIVED",
				QStringLiteral("bytes=%1").arg(text.toUtf8().size()));
		});

		connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMediaReceivingStarted,
			this, [this](const QString &transferId, const QString &senderIdentity, const QString &senderName,
						 const QString &mediaType, const QString &fileName, qint64 totalSize, int64_t seq) {
			const auto dispName = resolveParticipantDisplayName(
				_coordinator, senderIdentity, senderName);

			if (_chatSidebar) {
				_chatSidebar->startReceivingMedia(transferId, senderIdentity, dispName, mediaType, fileName, totalSize, seq);
			}

			if (_activeSidebar != ActiveSidebar::Chat && _bottomBar) {
				_bottomBar->setChatUnreadCount(_bottomBar->chatUnreadCount() + 1);
			}

			LogToConsole(LogCategory::Participant, "CHAT_MEDIA_RECEIVING",
				QStringLiteral("type=%1 bytes=%2")
					.arg(mediaType == QStringLiteral("image") ? QStringLiteral("image") : QStringLiteral("file"))
					.arg(totalSize));
		});

		connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMediaReceivingProgress,
			this, [this](const QString &transferId, int progress) {
			if (_chatSidebar) {
				_chatSidebar->updateReceivingProgress(transferId, progress);
			}
		});

		connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMediaReceivingCompleted,
			this, [this](const QString &transferId, const QString &senderIdentity, const QString &senderName,
						 const QString &mediaType, const QString &fileName, const QByteArray &data) {
			if (_chatSidebar) {
				_chatSidebar->completeReceivingMedia(transferId, mediaType, fileName, data);
			}

			LogToConsole(LogCategory::Participant, "CHAT_MEDIA_RECEIVED",
				QStringLiteral("type=%1 bytes=%2")
					.arg(mediaType == QStringLiteral("image") ? QStringLiteral("image") : QStringLiteral("file"))
					.arg(data.size()));
		});

		connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMediaReceivingFailed,
			this, [this](const QString &transferId, const QString &reason) {
			if (_chatSidebar) {
				_chatSidebar->failReceivingMedia(transferId, reason);
			}
			LogToConsole(LogCategory::Participant, "CHAT_MEDIA_FAILED",
				QStringLiteral("transfer_interrupted"));
		});

		connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMessageSendProgress,
			this, [this](const QString &messageId, int progress) {
			if (_chatSidebar) {
				_chatSidebar->updateMessageStatus(messageId, OpenMeeting::MessageSendStatus::Sending, progress);
			}
		});

		connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMessageSendSuccess,
			this, [this](const QString &messageId) {
			if (_chatSidebar) {
				_chatSidebar->updateMessageStatus(messageId, OpenMeeting::MessageSendStatus::Sent, 100);
			}
		});

		connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::chatMessageSendFailed,
			this, [this](const QString &messageId, const QString &error) {
			if (_chatSidebar) {
				_chatSidebar->updateMessageStatus(messageId, OpenMeeting::MessageSendStatus::Failed, 0, error);
			}
		});
	}

	_bottomBar->endMeetingClicked() | rpl::on_next([this] {
		handleEndMeetingClicked();
	}, lifetime());

	_bottomBar->sendChatRequested() | rpl::on_next([this](const QString &text) {
		_topBar->setActiveSpeaker(QString::fromUtf8("%1: %2").arg(_config.displayName).arg(text));
		LogToConsole(LogCategory::Participant, "CHAT_TEXT_SENT",
			QStringLiteral("bytes=%1").arg(text.toUtf8().size()));
		if (_coordinator) {
			_coordinator->sendChatMessage(text);
		}
	}, lifetime());

	_bottomBar->microphoneDeviceChanged() | rpl::on_next([this](const QString &devId) {
		auto &session = OpenMeeting::SessionManager::instance();
		auto preferences = session.mediaPreferences();
		if (preferences.microphoneDeviceId == devId) {
			requestMicrophoneSwitch(devId);
		} else {
			preferences.microphoneDeviceId = devId;
			session.setMediaPreferences(preferences);
		}
	}, lifetime());

	_bottomBar->speakerDeviceChanged() | rpl::on_next([this](const QString &deviceId) {
		auto &session = OpenMeeting::SessionManager::instance();
		auto preferences = session.mediaPreferences();
		if (preferences.speakerDeviceId == deviceId) {
			selectSpeakerDevice(deviceId);
		} else {
			preferences.speakerDeviceId = deviceId;
			session.setMediaPreferences(preferences);
		}
	}, lifetime());

	bindCameraDeviceChanges();

	_localGenTimer = new QTimer(this);
	connect(_localGenTimer, &QTimer::timeout, this, &MeetingRoomWindow::onLocalVideoGenerated);
}

void MeetingRoomWindow::applyAudioProcessingPreferences(
		const OpenMeeting::MediaPreferences &preferences) {
	if (!_wasapiCap) return;
	const auto processor = _wasapiCap->apm_processor();
	if (!processor) return;
	auto config = processor->GetConfig();
	if (config.enable_aec == preferences.echoCancellation
		&& config.enable_ans == preferences.noiseSuppression
		&& config.enable_agc == preferences.autoGainControl) return;
	config.enable_aec = preferences.echoCancellation;
	config.enable_ans = preferences.noiseSuppression;
	config.enable_agc = preferences.autoGainControl;
	// Keep the capture and render-reference paths on the same APM instance.
	// ApplyConfig serializes changes with audio callbacks internally.
	processor->ApplyConfig(config);
}

std::uint64_t MeetingRoomWindow::beginDeviceSwitchTelemetry(
		DeviceSwitchTelemetry &operation,
		livekit::telemetry::OperationKind kind) {
	if (!operation.operationId.empty()) {
		finishDeviceSwitchTelemetry(
			operation, operation.serial,
			livekit::telemetry::OperationOutcome::Cancelled);
	}
	++operation.serial;
	operation.kind = kind;
	operation.telemetry = _coordinator
		? _coordinator->sessionTelemetry()
		: std::weak_ptr<livekit::telemetry::SessionTelemetry>{};
	if (const auto telemetry = operation.telemetry.lock()) {
		operation.operationId = telemetry->StartOperation(kind);
	}
	return operation.serial;
}

void MeetingRoomWindow::finishDeviceSwitchTelemetry(
		DeviceSwitchTelemetry &operation,
		std::uint64_t serial,
		livekit::telemetry::OperationOutcome outcome) {
	if (serial == 0 || operation.serial != serial) return;
	if (const auto telemetry = operation.telemetry.lock();
		telemetry && !operation.operationId.empty()) {
		telemetry->FinishOperation(operation.operationId, operation.kind, outcome);
	}
	operation.operationId.clear();
	operation.telemetry.reset();
}

void MeetingRoomWindow::cancelDeviceSwitchTelemetry() {
	finishDeviceSwitchTelemetry(
		_cameraSwitchTelemetry, _cameraSwitchTelemetry.serial,
		livekit::telemetry::OperationOutcome::Cancelled);
	finishDeviceSwitchTelemetry(
		_microphoneSwitchTelemetry, _microphoneSwitchTelemetry.serial,
		livekit::telemetry::OperationOutcome::Cancelled);
	finishDeviceSwitchTelemetry(
		_speakerSwitchTelemetry, _speakerSwitchTelemetry.serial,
		livekit::telemetry::OperationOutcome::Cancelled);
	_pendingMicrophoneDeviceGeneration = 0;
}

bool MeetingRoomWindow::selectSpeakerDevice(const QString &deviceId) {
	const auto operation = beginDeviceSwitchTelemetry(
		_speakerSwitchTelemetry,
		livekit::telemetry::OperationKind::SpeakerDeviceSwitch);
	auto &manager = livekit::WebRTCManager::Instance();
	const bool selected = manager.SetPlayoutDeviceById(deviceId.toStdString());
	_speakerAvailable = manager.EnsurePlayout();
	if (!_speakerAvailable) setSpeakerOutputMuted(true);
	if (!selected || !_speakerAvailable) {
		finishDeviceSwitchTelemetry(
			_speakerSwitchTelemetry, operation,
			livekit::telemetry::OperationOutcome::Failure);
		LogToConsole(LogCategory::Error, "AUDIO_OUTPUT", "Unable to select speaker output device");
		return false;
	}
	if (_bottomBar) _bottomBar->setSpeakerDeviceId(deviceId);
	finishDeviceSwitchTelemetry(
		_speakerSwitchTelemetry, operation,
		livekit::telemetry::OperationOutcome::Success);
	return true;
}

void MeetingRoomWindow::requestMicrophoneSwitch(const QString &deviceId) {
	const auto operation = beginDeviceSwitchTelemetry(
		_microphoneSwitchTelemetry,
		livekit::telemetry::OperationKind::MicrophoneDeviceSwitch);
	if (!_wasapiCap) {
		finishDeviceSwitchTelemetry(
			_microphoneSwitchTelemetry, operation,
			livekit::telemetry::OperationOutcome::Failure);
		return;
	}
	const auto generation = _wasapiCap->SwitchDeviceTracked(deviceId.toStdString());
	if (generation == 0) {
		finishDeviceSwitchTelemetry(
			_microphoneSwitchTelemetry, operation,
			livekit::telemetry::OperationOutcome::Failure);
		return;
	}
	_pendingMicrophoneDeviceGeneration = generation;
	if (_wasapiCap->IsRunning() &&
		_wasapiCap->activeDeviceGeneration() == generation) {
		finishDeviceSwitchTelemetry(
			_microphoneSwitchTelemetry, operation,
			livekit::telemetry::OperationOutcome::Success);
		_pendingMicrophoneDeviceGeneration = 0;
		return;
	}
	QPointer<MeetingRoomWindow> owner(this);
	QTimer::singleShot(5000, this, [owner, operation, generation] {
		if (!owner || owner->_pendingMicrophoneDeviceGeneration != generation) return;
		owner->_pendingMicrophoneDeviceGeneration = 0;
		owner->finishDeviceSwitchTelemetry(
			owner->_microphoneSwitchTelemetry, operation,
			livekit::telemetry::OperationOutcome::Timeout);
	});
}

void MeetingRoomWindow::setSpeakerOutputMuted(bool muted) {
	if (!muted) {
		_speakerAvailable = livekit::WebRTCManager::Instance().EnsurePlayout();
		muted = !_speakerAvailable;
	}
	if (_bottomBar) _bottomBar->setSpeakerMuted(muted);
	if (_room) _room->SetAudioOutputMuted(muted);
}

void MeetingRoomWindow::applyMicrophoneAvailability(bool available) {
	_microphoneAvailable = available;
	if (_coordinator) _coordinator->setLocalAudioAvailable(available);
}

void MeetingRoomWindow::bindMicrophoneCaptureState() {
	const std::weak_ptr<livekit::WasapiAudioCapture> capture = _wasapiCap;
	if (!_captureUiCallbacks) {
		_captureUiCallbacks = OpenMeeting::QtCallbackGate<MeetingRoomWindow>::Create(this);
	}
	const auto callbacks = _captureUiCallbacks;
	_wasapiCap->SetCaptureStateCallback([callbacks, capture](bool available) {
		callbacks->Post([capture, available](MeetingRoomWindow *window) {
			if (!window->_sessionRunning || window->_closingForSessionInvalidation) return;
			const auto source = capture.lock();
			if (!source || window->_wasapiCap != source) return;
			window->applyMicrophoneAvailability(available);
		});
	});
	_wasapiCap->SetDeviceFrameCallback([callbacks, capture](std::uint64_t generation) {
		callbacks->Post([capture, generation](MeetingRoomWindow *window) {
			if (!window->_sessionRunning ||
				window->_closingForSessionInvalidation) return;
			const auto source = capture.lock();
			if (!source || window->_wasapiCap != source ||
				window->_pendingMicrophoneDeviceGeneration != generation) return;
			window->_pendingMicrophoneDeviceGeneration = 0;
			window->finishDeviceSwitchTelemetry(
				window->_microphoneSwitchTelemetry,
				window->_microphoneSwitchTelemetry.serial,
				livekit::telemetry::OperationOutcome::Success);
		});
	});
}

void MeetingRoomWindow::setupAudioPreferencesBinding(
		OpenMeeting::SessionManager &sessionManager) {
	applyAudioProcessingPreferences(sessionManager.mediaPreferences());
	if (_audioPreferencesBound) return;
	_audioPreferencesBound = true;
	if (_bottomBar) {
		_bottomBar->setSpeakerDeviceId(sessionManager.mediaPreferences().speakerDeviceId);
		_bottomBar->setMicrophoneDeviceId(sessionManager.mediaPreferences().microphoneDeviceId);
	}
	connect(&sessionManager, &OpenMeeting::SessionManager::preferencesChanged, this,
		[this, session = QPointer<OpenMeeting::SessionManager>(&sessionManager),
			previousMicrophone = sessionManager.mediaPreferences().microphoneDeviceId,
			previousSpeaker = sessionManager.mediaPreferences().speakerDeviceId]
		(const OpenMeeting::MediaPreferences &preferences) mutable {
			applyAudioProcessingPreferences(preferences);
			if (previousMicrophone != preferences.microphoneDeviceId) {
				previousMicrophone = preferences.microphoneDeviceId;
				if (_bottomBar) _bottomBar->setMicrophoneDeviceId(previousMicrophone);
				requestMicrophoneSwitch(previousMicrophone);
			}
			if (previousSpeaker != preferences.speakerDeviceId) {
				if (_room && _coordinator
					&& _coordinator->state() == OpenMeeting::MeetingState::InMeeting) {
					if (!selectSpeakerDevice(preferences.speakerDeviceId)) {
						// Restore the previous choice after the current settings commit
						// finishes; never overwrite a newer user selection.
						QMetaObject::invokeMethod(this,
							[session, requested = preferences.speakerDeviceId, previousSpeaker] {
								if (!session || session->mediaPreferences().speakerDeviceId != requested) return;
								auto restored = session->mediaPreferences();
								restored.speakerDeviceId = previousSpeaker;
								session->setMediaPreferences(restored);
							}, Qt::QueuedConnection);
						return;
					}
				}
				previousSpeaker = preferences.speakerDeviceId;
			}
		});
}

void MeetingRoomWindow::setupCameraCompletionOwner(
		OpenMeeting::SessionManager &sessionManager) {
	Q_ASSERT(thread() == QThread::currentThread());
	Q_ASSERT(sessionManager.thread() == thread());
	_cameraSessionManager = &sessionManager;
	if (!_cameraLogEffect) {
		_cameraLogEffect = [](bool error, const QString &tag, const QString &message) {
			LogToConsole(error ? LogCategory::Error : LogCategory::Media, tag, message);
		};
	}
	if (!_cameraWarningEffect) {
		_cameraWarningEffect = [](QWidget *parent, const QString &title, const QString &message) {
			QMessageBox::warning(parent, title, message);
		};
	}

	QPointer<MeetingRoomWindow> window(this);
	_cameraCompletionOwner = std::make_unique<CameraSwitchCompletionOwner>(
		this,
		[window](const CameraSwitchCompletionOwner::Ticket &ticket,
		         const QString &devicePath,
		         bool success,
		         const std::string &error) {
			if (window) {
				window->handleCameraSwitchResult(ticket, devicePath, success, error);
			}
		});

	connect(&sessionManager,
	        &OpenMeeting::SessionManager::sessionInvalidated,
	        this,
	        [this](OpenMeeting::SessionInvalidationReason) {
			cancelMediaPreparation();
			invalidateCameraCompletion();
		});
	connect(&sessionManager,
	        &OpenMeeting::SessionManager::sessionInvalidated,
	        this,
	        &MeetingRoomWindow::onSessionInvalidated,
	        Qt::QueuedConnection);
	connect(&sessionManager, &OpenMeeting::SessionManager::loggedOut, this,
		[this, &sessionManager]() {
			cancelMediaPreparation();
			invalidateCameraCompletion();
			if (!sessionManager.isSessionInvalidating())
				onSessionInvalidated(OpenMeeting::SessionInvalidationReason::UserLogout);
		});
}

void MeetingRoomWindow::bindCameraDeviceChanges() {
	Q_ASSERT(_bottomBar != nullptr);
	_bottomBar->videoDeviceChanged() | rpl::on_next([this](const QString &devicePath) {
		requestCameraSwitch(devicePath);
	}, lifetime());
}

void MeetingRoomWindow::requestCameraSwitch(const QString &devicePath) {
	Q_ASSERT(thread() == QThread::currentThread());
	if (!_cameraCompletionOwner || !_cameraSessionManager) return;
	if (!_cameraManager) {
		// Selecting a device while capture is deferred must not open hardware.
		if (_cameraSessionManager->isSessionInvalidating()) return;
		auto preferences = _cameraSessionManager->mediaPreferences();
		preferences.cameraDeviceId = devicePath;
		_cameraSessionManager->setMediaPreferences(preferences);
		_currentCameraPath = devicePath;
		return;
	}
	if (_cameraSessionManager->isSessionInvalidating()) {
		invalidateCameraCompletion();
		return;
	}

	_currentCameraPath = devicePath;
	const auto telemetryOperation = beginDeviceSwitchTelemetry(
		_cameraSwitchTelemetry,
		livekit::telemetry::OperationKind::CameraDeviceSwitch);
	const auto ticket = _cameraCompletionOwner->beginRequest(devicePath);
	if (!ticket) {
		finishDeviceSwitchTelemetry(
			_cameraSwitchTelemetry, telemetryOperation,
			livekit::telemetry::OperationOutcome::Failure);
		return;
	}
	auto manager = _cameraManager;
	auto callback = _cameraCompletionOwner->makeCallback(ticket);
	const auto logEffect = _cameraLogEffect;
	QPointer<MeetingRoomWindow> guard(this);
	if (logEffect) {
		logEffect(false, "CAMERA_SWITCH",
			QStringLiteral("camera_switch_requested device=%1")
				.arg(devicePath.isEmpty() ? QStringLiteral("default") : QStringLiteral("selected")));
	}
	if (!guard || !ticket.isCurrent()) {
		finishDeviceSwitchTelemetry(
			_cameraSwitchTelemetry, telemetryOperation,
			livekit::telemetry::OperationOutcome::Cancelled);
		return;
	}
	if (!guard->_cameraSessionManager
		|| guard->_cameraSessionManager->isSessionInvalidating()) {
		guard->invalidateCameraCompletion();
		return;
	}
	manager->SwitchDeviceAsync(
		devicePath.toStdString(),
		3000,
		std::move(callback));
}

void MeetingRoomWindow::handleCameraSwitchResult(
		const CameraSwitchCompletionOwner::Ticket &ticket,
		const QString &devicePath,
		bool success,
		const std::string &error) {
	Q_ASSERT(thread() == QThread::currentThread());
	if (!ticket.isCurrent() || !_cameraSessionManager) {
		return;
	}
	if (_cameraSessionManager->isSessionInvalidating()) {
		invalidateCameraCompletion();
		return;
	}

	const auto logEffect = _cameraLogEffect;
	const auto warningEffect = _cameraWarningEffect;
	QPointer<MeetingRoomWindow> guard(this);
	if (success) {
		finishDeviceSwitchTelemetry(
			_cameraSwitchTelemetry, _cameraSwitchTelemetry.serial,
			livekit::telemetry::OperationOutcome::Success);
		_usingRealCamera = true;
		if (_coordinator) _coordinator->setLocalVideoAvailable(true);
		if (_localTile) {
			_localTile->setVideoActive(_config.videoEnabled && _usingRealCamera);
		}
		if (logEffect) {
			logEffect(false, "CAMERA_SWITCH",
				QStringLiteral("camera_switch_succeeded device=%1")
					.arg(devicePath.isEmpty() ? QStringLiteral("default") : QStringLiteral("selected")));
		}
		return;
	}

	const auto errorText = QString::fromStdString(error);
	finishDeviceSwitchTelemetry(
		_cameraSwitchTelemetry, _cameraSwitchTelemetry.serial,
		errorText.contains(QStringLiteral("timeout"), Qt::CaseInsensitive)
			? livekit::telemetry::OperationOutcome::Timeout
			: livekit::telemetry::OperationOutcome::Failure);
	if (logEffect) {
		logEffect(true, "CAMERA_SWITCH",
			QStringLiteral("camera_switch_failed previous_device_restored"));
	}
	if (!guard || !ticket.isCurrent() || !guard->_cameraSessionManager
		|| guard->_cameraSessionManager->isSessionInvalidating()) {
		return;
	}
	if (warningEffect) {
		warningEffect(
			guard,
			QCoreApplication::translate("MeetingUI", "Camera Switch Failed"),
			QCoreApplication::translate("MeetingUI", "Unable to start the selected camera. Capture continues on the previous device.\nReason: %1")
				.arg(errorText));
	}
}

void MeetingRoomWindow::invalidateCameraCompletion() {
	if (_cameraCompletionOwner) {
		_cameraCompletionOwner->invalidate();
	}
	finishDeviceSwitchTelemetry(
		_cameraSwitchTelemetry, _cameraSwitchTelemetry.serial,
		livekit::telemetry::OperationOutcome::Cancelled);
}

void MeetingRoomWindow::retireLocalCapture() {
	Q_ASSERT(thread() == QThread::currentThread());
	auto microphone = std::move(_wasapiCap);
	auto manager = std::move(_cameraManager);
	auto legacyCapture = std::move(_dshowCap);
	auto audioSource = std::move(_localAudioSource);
	auto videoSource = std::move(_localVideoSource);
	if (!microphone && !manager && !legacyCapture && !audioSource && !videoSource) return;
	// The application-owned worker keeps the devices and their output sources
	// alive through Stop, even if the meeting window has already been deleted.
	OpenMeeting::SessionShutdownService::Instance().SubmitCleanup(
		[microphone = std::move(microphone), manager = std::move(manager),
		 legacyCapture = std::move(legacyCapture), audioSource = std::move(audioSource),
		 videoSource = std::move(videoSource)]() mutable {
			// SessionShutdownService runs and releases native owners in an MTA.
			if (microphone) microphone->Stop();
			if (manager) manager->Stop();
			if (legacyCapture) legacyCapture->Stop();
			microphone.reset();
			manager.reset();
			legacyCapture.reset();
			audioSource.reset();
			videoSource.reset();
		});
}

void MeetingRoomWindow::onTimerTick() {
	_elapsedSeconds++;
	_topBar->updateDuration(_elapsedSeconds);
}

void MeetingRoomWindow::onRemoteRenderTick() {
	if (_remoteRenderSession && isVideoStageVisible()) {
		_remoteRenderSession->RenderLatestFrames();
		const auto nowMs = QDateTime::currentMSecsSinceEpoch();
		if (nowMs - _lastRenderTelemetrySampleMs >= 1000) {
			_lastRenderTelemetrySampleMs = nowMs;
			if (const auto telemetry = _coordinator
					? _coordinator->sessionTelemetry().lock() : nullptr) {
				const auto statistics = _remoteRenderSession->statistics();
				if (_videoCanvas) _renderDiagnostics = _videoCanvas->renderDiagnostics();
				livekit::telemetry::RenderPipelineSample sample;
				sample.router_submitted = statistics.router.submitted;
				sample.router_replaced_before_render =
					statistics.router.replaced_before_render;
				sample.router_rejected_generation =
					statistics.router.rejected_generation;
				sample.router_rejected_binding =
					statistics.router.rejected_binding;
				sample.router_dropped_invalid = statistics.router.dropped_invalid;
				sample.router_dropped_capacity = statistics.router.dropped_capacity;
				sample.delivered_to_gpu = statistics.delivered_to_gpu;
				sample.delivered_to_qt_cpu = statistics.delivered_to_qt_cpu;
				sample.qt_cpu_conversion_failures =
					statistics.qt_cpu_conversion_failures;
				sample.rejected_track_attachments =
					statistics.rejected_track_attachments;
				sample.attached_track_count = statistics.attached_track_count;
				sample.requested_backend = livekit::render::RenderBackendName(
					_renderDiagnostics.requested_backend).toStdString();
				sample.actual_backend = livekit::render::RenderBackendName(
					_renderDiagnostics.actual_backend).toStdString();
				sample.gpu_failure = livekit::render::RenderGpuFailureName(
					_renderDiagnostics.gpu_failure).toStdString();
				sample.fallback_reason = livekit::render::RenderFallbackReasonName(
					_renderDiagnostics.fallback_reason).toStdString();
				telemetry->RecordRenderPipelineSample(std::move(sample));
			}
		}
	}
	if (_localScreenTile && _localScreenPreview && isVideoStageVisible()) {
		auto frame = _localScreenPreview->TakeLatest("screen", _localScreenPreview->generation());
		if (!frame) return;
		_localScreenTile->setVideoActive(true);
        if (_remoteRenderSession) _remoteRenderSession->RenderFrame(
            _localScreenTile->renderKey().toStdString(),
            livekit::render::VideoRenderFrame::FromI420(std::move(frame)));
	} else if (_localScreenPreview) {
		(void)_localScreenPreview->TakeLatest(
			"screen", _localScreenPreview->generation());
	}
}

void MeetingRoomWindow::switchSidebar(ActiveSidebar target) {
	if (_activeSidebar == target) {
		_activeSidebar = ActiveSidebar::None;
	} else {
		_activeSidebar = target;
	}

	if (_activeSidebar == ActiveSidebar::Chat && _bottomBar) {
		_bottomBar->setChatUnreadCount(0);
	}

	QResizeEvent ev(size(), size());
	resizeEvent(&ev);
}

void MeetingRoomWindow::updateRecoveryStateUi(OpenMeeting::MeetingState state, const QString &detail) {
	if (state == OpenMeeting::MeetingState::Reconnecting) {
		setAnnotationInteractionEnabled(false);
	} else if (state == OpenMeeting::MeetingState::InMeeting) {
		setAnnotationInteractionEnabled(true);
	} else {
		closeAnnotationOverlay();
	}
	if (!_recoveryBanner) return;

	const int stageW = _stageContainer ? _stageContainer->width() : width();


	if (_preparingMedia) {
		if (_recoveryBannerFadeTimer) _recoveryBannerFadeTimer->stop();
		MeetingUI::AppTheme::setStyleVariant(*_recoveryBanner, "meeting-room-window-recoverybanner");
		_recoveryBanner->setText(QCoreApplication::translate("MeetingUI", "Preparing audio and video devices..."));
		_recoveryBanner->show();
		_recoveryBanner->raise();
		if (_bottomBar) _bottomBar->setInRecovery(true);
	} else switch (state) {
	case OpenMeeting::MeetingState::Validating:
	case OpenMeeting::MeetingState::ConnectingRoom:
	case OpenMeeting::MeetingState::StartingLocalMedia: {
		if (_recoveryBannerFadeTimer) _recoveryBannerFadeTimer->stop();
		MeetingUI::AppTheme::setStyleVariant(*_recoveryBanner, "meeting-room-window-recoverybanner");
		_recoveryBanner->setText(QCoreApplication::translate("MeetingUI", "🔄 Connecting to the meeting..."));
		_recoveryBanner->show();
		_recoveryBanner->raise();
		if (_bottomBar) _bottomBar->setInRecovery(true);
		break;
	}
	case OpenMeeting::MeetingState::Reconnecting: {
		if (_recoveryBannerFadeTimer) _recoveryBannerFadeTimer->stop();
		_wasReconnecting = true;
		MeetingUI::AppTheme::setStyleVariant(*_recoveryBanner, "meeting-room-window-recoverybanner-2");
		_recoveryBanner->setText(QCoreApplication::translate("MeetingUI", "⚠️ Connection interrupted. Reconnecting to the meeting..."));
		_recoveryBanner->show();
		_recoveryBanner->raise();
		if (_bottomBar) _bottomBar->setInRecovery(true);
		break;
	}
	case OpenMeeting::MeetingState::InMeeting: {
		if (_bottomBar) _bottomBar->setInRecovery(false);
		if (_wasReconnecting) {
			_wasReconnecting = false;
			MeetingUI::AppTheme::setStyleVariant(*_recoveryBanner, "meeting-room-window-recoverybanner-3");
			_recoveryBanner->setText(QCoreApplication::translate("MeetingUI", "✅ Meeting connection restored"));
			_recoveryBanner->show();
			_recoveryBanner->raise();
			if (_recoveryBannerFadeTimer) {
				_recoveryBannerFadeTimer->start(1500);
			}
		} else {
			if (_recoveryBannerFadeTimer) _recoveryBannerFadeTimer->stop();
			_recoveryBanner->hide();
		}
		break;
	}
	case OpenMeeting::MeetingState::Failed: {
		if (_bottomBar) _bottomBar->setInRecovery(false);
		_wasReconnecting = false;
		if (_recoveryBannerFadeTimer) _recoveryBannerFadeTimer->stop();
		MeetingUI::AppTheme::setStyleVariant(*_recoveryBanner, "meeting-room-window-recoverybanner-4");
		_recoveryBanner->setText(QCoreApplication::translate("MeetingUI", "❌ Meeting connection failed: %1").arg(detail.isEmpty() ? QCoreApplication::translate("MeetingUI", "Network or authentication error") : detail));
		_recoveryBanner->show();
		_recoveryBanner->raise();
		break;
	}
	case OpenMeeting::MeetingState::Idle:
	default: {
		if (_bottomBar) _bottomBar->setInRecovery(false);
		_wasReconnecting = false;
		if (_recoveryBannerFadeTimer) _recoveryBannerFadeTimer->stop();
		_recoveryBanner->hide();
		break;
	}
	}
 const auto size = bannerSize(*_recoveryBanner, stageW, 420);
 _recoveryBanner->setGeometry(QRect(QPoint((stageW - size.width()) / 2, 16), size));
    if (_videoCanvas) _videoCanvas->setStageOverlay(_recoveryBanner);
}

void MeetingRoomWindow::resizeEvent(QResizeEvent *e) {
	const int w = width();
	const int h = height();

 const int topBarH = _topBar->heightForWidth(w);
 const int bottomBarH = _bottomBar->heightForWidth(w);
 _topBar->setGeometry(0, 0, w, topBarH);

 QWidget *sidebar = _activeSidebar == ActiveSidebar::Participants
     ? static_cast<QWidget *>(_participantsSidebar) : static_cast<QWidget *>(_chatSidebar);
 const int sidebarW = _activeSidebar != ActiveSidebar::None && sidebar
     ? std::min(w / 2, std::max(340, sidebar->minimumSizeHint().width())) : 0;
	const int stageW = w - sidebarW;
    const int annotationW = _annotationButton && !_annotationButton->isHidden()
        ? std::max(80, _annotationButton->sizeHint().width()) : 0;
    const int controlStopW = _remoteControlStopButton && !_remoteControlStopButton->isHidden()
        ? std::max(100, _remoteControlStopButton->sizeHint().width()) : 0;
    if (_screenShareBanner) _screenShareBanner->setContentsMargins(136, 4,
        12 + (annotationW ? annotationW + 8 : 0) + (controlStopW ? controlStopW + 8 : 0), 4);
	const int shareBannerH = _screenShareBanner && !_screenShareBanner->isHidden() ? std::max(44, _screenShareBanner->heightForWidth(w)) : 0;
	const int encryptionH = _encryptionPanel ? std::max(44, _encryptionPanel->sizeHint().height()) : 0;
	if (_encryptionPanel) _encryptionPanel->setGeometry(0, topBarH + shareBannerH, w, encryptionH);
	const int stageTop = topBarH + shareBannerH + encryptionH;
	const int stageH = std::max(0, h - stageTop - bottomBarH);
	if (_screenShareBanner) _screenShareBanner->setGeometry(0, topBarH, w, shareBannerH);
    if (_screenQualityButton && shareBannerH > 0) {
        _screenQualityButton->setGeometry(12, (shareBannerH - 32) / 2, 116, 32);
        _screenQualityButton->raise();
    }
	if (_annotationButton && _annotationButton->isVisible() && shareBannerH > 0) {
		_annotationButton->adjustSize();
		const auto hint = _annotationButton->sizeHint();
		const int buttonHeight = std::min(hint.height(), std::max(1, shareBannerH - 8));
		const int buttonWidth = std::min(hint.width(), std::max(1, w - 24));
		_annotationButton->setGeometry(
			std::max(12, w - buttonWidth - 12),
			std::max(0, (shareBannerH - buttonHeight) / 2),
			buttonWidth,
			buttonHeight);
		_annotationButton->raise();
	}
    if (controlStopW && shareBannerH > 0) {
        _remoteControlStopButton->setGeometry(w - 12 - (annotationW ? annotationW + 8 : 0) - controlStopW,
            (shareBannerH - 32) / 2, controlStopW, 32);
        _remoteControlStopButton->raise();
    }

	_stageContainer->setGeometry(0, stageTop, stageW, stageH);

	if (_activeSidebar == ActiveSidebar::Participants) {
		if (_participantsSidebar) {
			_participantsSidebar->setGeometry(stageW, stageTop, sidebarW, stageH);
			_participantsSidebar->show();
			_participantsSidebar->raise();
		}
		if (_chatSidebar) {
			_chatSidebar->hide();
		}
	} else if (_activeSidebar == ActiveSidebar::Chat) {
		if (_chatSidebar) {
			_chatSidebar->setGeometry(stageW, stageTop, sidebarW, stageH);
			_chatSidebar->show();
			_chatSidebar->raise();
		}
		if (_participantsSidebar) {
			_participantsSidebar->hide();
		}
	} else {
		if (_participantsSidebar) _participantsSidebar->hide();
		if (_chatSidebar) _chatSidebar->hide();
	}

	_bottomBar->setGeometry(0, h - bottomBarH, w, bottomBarH);

	updateVideoLayout();
	scheduleViewportIntent(false);
}

void MeetingRoomWindow::onRemoteParticipantJoined(const QString &identity, const QString &name) {
	applyRemoteParticipantJoined(identity, name, nullptr);
}

void MeetingRoomWindow::applyRemoteParticipantJoined(const QString &identity, const QString &name,
		const OpenMeeting::ParticipantPresentation *presentation) {
	if (identity.isEmpty()) return;
	QPointer<MeetingRoomWindow> owner(this);
	QPointer<OpenMeeting::MeetingCoordinator> coordinator(_coordinator.get());
	const auto current = [&] {
		return owner && (!presentation || (coordinator && owner->_coordinator.get() == coordinator &&
			coordinator->isParticipantPresentationCurrent(*presentation)));
	};
	if (!current()) return;

	QString dispName = name.isEmpty() ? identity : name;
	if (!presentation && dispName == identity && _coordinator) {
		for (const auto &p : _coordinator->participants()) {
			if (p.identity == identity && !p.name.isEmpty()) {
				dispName = p.name;
				break;
			}
		}
	}
	_remoteParticipantNames[identity] = dispName;

	auto it = _remoteTiles.find(identity);
	if (it == _remoteTiles.end()) {
		const bool hasVisibleCamera = std::any_of(
			_acceptedVideoPlan.visible_seats.begin(),
			_acceptedVideoPlan.visible_seats.end(),
			[&](const auto &seat) {
				return seat.source != livekit::TrackSource::ScreenShareVideo &&
					QString::fromStdString(seat.key.participant.identity.empty()
						? seat.key.participant.sid
						: seat.key.participant.identity) == identity;
			});
		if (!hasVisibleCamera) {
			_participantCount = 1 + static_cast<int>(_remoteParticipantNames.size());
			if (_bottomBar) _bottomBar->setParticipantCount(_participantCount);
			return;
		}
		QPointer<VideoTileWidget> tile(new VideoTileWidget(dispName, false, _stageContainer));
		if (!current()) {
			if (tile) delete tile.data();
			return;
		}
		_remoteTiles[identity].reset(tile.data());
		tile->setIdentity(identity);
		tile->setRenderKey(QStringLiteral("remote-camera/") + identity);
		if (!current() || !tile) return;
		tile->setVideoActive(false);
		if (!current() || !tile) return;

		bindTileInteractions(tile.data());

		connect(tile.data(), &VideoTileWidget::remoteVolumeChanged, [this, identity](float volume) {
			_remoteVolumes[identity] = volume;
			if (_room) {
				_room->SetParticipantVolume(identity.toStdString(), volume);
			}
		});

		connect(tile.data(), &VideoTileWidget::remoteLocalMuteToggled, [this, identity](bool muted) {
			if (muted) _locallyMutedUsers.insert(identity);
			else _locallyMutedUsers.erase(identity);
			if (_room) {
				_room->SetParticipantMuted(identity.toStdString(), muted);
			}
		});

		tile->show();
		if (!current() || !tile) return;
	} else {
		if (!dispName.isEmpty() && it->second && it->second->displayName() != dispName) {
			it->second->setDisplayName(dispName);
			if (!current()) return;
		}
	}

	if (!current()) return;
	if (_room) {
		auto itVol = _remoteVolumes.find(identity);
		if (itVol != _remoteVolumes.end()) {
			_room->SetParticipantVolume(identity.toStdString(), itVol->second);
			if (!current()) return;
		}
		if (_locallyMutedUsers.count(identity)) {
			_room->SetParticipantMuted(identity.toStdString(), true);
			if (!current()) return;
		}
	}

	if (auto tileIt = _remoteTiles.find(identity);
		tileIt != _remoteTiles.end() && tileIt->second && _coordinator) {
		const auto participants = presentation
			? std::vector<OpenMeeting::ParticipantInfo>{presentation->participant}
			: _coordinator->participants();
		QPointer<VideoTileWidget> tile(tileIt->second.get());
		for (const auto &participant : participants) {
			if (participant.identity != identity) continue;
			if (!current() || !tile) return;
			tile->setAudioMuted(participant.isAudioMuted);
			if (!current() || !tile) return;
			tile->setConnectionQuality(participant.connectionQuality);
			if (!current() || !tile) return;
			break;
		}
	}

	_participantCount = 1 + static_cast<int>(_remoteParticipantNames.size());
	if (_bottomBar) {
		_bottomBar->setParticipantCount(_participantCount);
		if (!current()) return;
	}

	LogToConsole(LogCategory::Participant, "USER_JOIN", QCoreApplication::translate("MeetingUI", "Remote participant joined: %1 (name: %2, participants: %3)").arg(identity).arg(dispName).arg(_participantCount));
	if (!current()) return;
	updateVideoLayout();
}

void MeetingRoomWindow::onRemoteParticipantLeft(const QString &identity) {
	std::vector<QString> retired;
	for (const auto &[sid, binding] : _remoteVideoBindings) {
		if (binding.identity == identity) retired.push_back(sid);
	}
	for (const auto &sid : retired) {
		removeRemoteVideo(sid);
		_remoteScreenTiles.erase(sid);
	}
	_remoteParticipantNames.erase(identity);
	if (_remoteRenderSession) {
		_remoteRenderSession->RemoveTracksForIdentity(identity.toStdString());
	}
	if (_videoCanvas) {
		_videoCanvas->removeUser((QStringLiteral("remote-camera/") + identity).toStdString());
	}
	auto it = _remoteTiles.find(identity);
	if (it != _remoteTiles.end()) {
		it->second->hide();
		_remoteTiles.erase(it);
	}

	if (_pinnedRenderKey == QStringLiteral("remote-camera/") + identity) {
		_pinnedRenderKey.clear();
	}

	_participantCount = 1 + static_cast<int>(_remoteParticipantNames.size());
	if (_bottomBar) {
		_bottomBar->setParticipantCount(_participantCount);
	}

	LogToConsole(LogCategory::Participant, "USER_LEFT", QCoreApplication::translate("MeetingUI", "Remote participant left: %1 (participants: %2)").arg(identity).arg(_participantCount));
	updateVideoLayout();
}


void MeetingRoomWindow::onRemoteTrackMuted(const QString &identity, bool isVideo, bool muted) {
	if (isVideo) {
		refreshRemoteVideoPresentations();
		return;
	}
	auto it = _remoteTiles.find(identity);
	if (it == _remoteTiles.end() || !it->second) return;
	it->second->setAudioMuted(muted);
	if (muted) it->second->setSpeaking(false, 0.0f);
	updateVideoLayout();
}

void MeetingRoomWindow::updateActiveSpeakers(const std::vector<livekit::ActiveSpeakerInfo> &speakers) {
	QString primarySpeakerName;
	std::unordered_map<std::string, float> speaking_levels;
	bool localSpeaking = false;
	float localLevel = 0.0f;

	for (const auto &spk : speakers) {
		if (spk.speaking) {
			speaking_levels[spk.sid] = spk.audio_level;
			speaking_levels[spk.identity] = spk.audio_level;
			if (spk.is_local) {
				localSpeaking = true;
				localLevel = spk.audio_level;
			}
			if (primarySpeakerName.isEmpty()) {
				primarySpeakerName = QString::fromStdString(spk.identity);
			}
		}
	}

	// 1. 顶部状态栏提示更新
	if (_topBar) {
		const auto &preferences = OpenMeeting::SessionManager::instance().mediaPreferences();
		_topBar->setActiveSpeaker(preferences.showActiveSpeaker ? primarySpeakerName : QString());
	}

	// 2. 本端画框发光光圈联动
	if (_localTile) {
		if (_config.audioMuted) {
			localSpeaking = false;
		}
		_localTile->setSpeaking(localSpeaking, localLevel);
	}

	// 3. 所有远端画框发光光圈联动
	for (auto &[id, tile] : _remoteTiles) {
		if (tile) {
			bool remoteSpeaking = false;
			float remoteLevel = 0.0f;
			auto it = speaking_levels.find(id.toStdString());
			if (it != speaking_levels.end()) {
				remoteSpeaking = true;
				remoteLevel = it->second;
			}
			tile->setSpeaking(remoteSpeaking, remoteLevel);
		}
	}
}

void MeetingRoomWindow::tryActivateGpuBackend() {
	if (_whiteboardVisible || !_videoCanvas || !_remoteRenderSession || !_remoteRenderSession->active() ||
        _usingGpuBackend.load(std::memory_order_acquire)) return;
    if (_gpuBackendActivationAttempted) {
        if (!_videoCanvas->rendererReady()) return;
    } else {
        _gpuBackendActivationAttempted = true;
        setupVideoCanvasInteractions();
        _videoCanvas->setGeometry(_stageContainer->rect());
        _videoCanvas->show();
    }

	if (!_videoCanvas->rendererReady()) {
        if (_videoCanvas->rendererPending()) return; // Qt initializes the GL context asynchronously.
		_renderDiagnostics = _videoCanvas->renderDiagnostics();
		_videoCanvas->shutdownRenderer();
		_videoCanvas->hide();
		_remoteRenderSession->UseQtCpuBackend();
		_renderDiagnostics = _videoCanvas->renderDiagnostics();
		livekit::diagnostic::Event event;
		event.kind = livekit::diagnostic::EventKind::RenderBackendChanged;
		event.thread_role = livekit::diagnostic::ThreadRole::Ui;
		event.from_render_backend = livekit::diagnostic::RenderBackend::Gpu;
		event.to_render_backend = livekit::diagnostic::RenderBackend::QtCpu;
		event.render_reason = livekit::diagnostic::RenderReason::InitializationFailed;
		if (_coordinator) event.context = _coordinator->diagnosticContext();
		livekit::diagnostic::EmitBusinessEvent(event);
		LogToConsole(LogCategory::WebRTC, "RENDER", QCoreApplication::translate("MeetingUI", "GPU canvas initialization failed. Using the Qt CPU video backend: ") +
			livekit::render::RenderDiagnosticsSafeSummary(_renderDiagnostics));
		return;
	}

	_remoteRenderSession->UseGpuBackend(
        [this](const std::string& key, livekit::render::VideoRenderFrame::Ptr frame) {
            receiveGpuVideoFrame(key, std::move(frame));
        });
	_usingGpuBackend.store(true, std::memory_order_release);
	_renderDiagnostics = _videoCanvas->renderDiagnostics();
	livekit::diagnostic::Event event;
	event.kind = livekit::diagnostic::EventKind::RenderBackendChanged;
	event.thread_role = livekit::diagnostic::ThreadRole::Ui;
	event.from_render_backend = livekit::diagnostic::RenderBackend::QtCpu;
	event.to_render_backend = livekit::diagnostic::RenderBackend::Gpu;
	event.render_reason = livekit::diagnostic::RenderReason::GpuReady;
	if (_coordinator) event.context = _coordinator->diagnosticContext();
	livekit::diagnostic::EmitBusinessEvent(event);
	LogToConsole(LogCategory::WebRTC, "RENDER", QCoreApplication::translate("MeetingUI", "GPU video backend enabled: ") +
		livekit::render::RenderDiagnosticsSafeSummary(_renderDiagnostics));
	updateVideoLayout();
}

void MeetingRoomWindow::fallBackToQtCpuBackend() {
	const bool was_using_gpu = _usingGpuBackend.exchange(false, std::memory_order_acq_rel);
	if (_remoteRenderSession) {
		_remoteRenderSession->UseQtCpuBackend();
	}
	if (_videoCanvas) {
		_renderDiagnostics = _videoCanvas->renderDiagnostics();
		_videoCanvas->shutdownRenderer();
		_videoCanvas->hide();
		_renderDiagnostics = _videoCanvas->renderDiagnostics();
	}
	if (was_using_gpu) {
		livekit::diagnostic::Event event;
		event.kind = livekit::diagnostic::EventKind::RenderBackendChanged;
		event.thread_role = livekit::diagnostic::ThreadRole::Ui;
		event.from_render_backend = livekit::diagnostic::RenderBackend::Gpu;
		event.to_render_backend = livekit::diagnostic::RenderBackend::QtCpu;
		event.render_reason = livekit::diagnostic::RenderReason::DeviceFailure;
		if (_coordinator) event.context = _coordinator->diagnosticContext();
		livekit::diagnostic::EmitBusinessEvent(event);
		LogToConsole(LogCategory::Error, "RENDER", QCoreApplication::translate("MeetingUI", "GPU presentation or device failure. Switched to the Qt CPU video backend: ") +
			livekit::render::RenderDiagnosticsSafeSummary(_renderDiagnostics));
	}
	updateVideoLayout();
}

void MeetingRoomWindow::syncVideoCanvasLayout(const std::vector<VideoTileWidget*> &tiles) {
	if (_whiteboardVisible || !_usingGpuBackend.load(std::memory_order_acquire) || !_videoCanvas) {
		return;
	}

	std::vector<livekit::render::VideoTileRect> canvas_tiles;
	canvas_tiles.reserve(tiles.size());
	for (auto *tile : tiles) {
		if (!tile) continue;
		const QRect geometry = tile->geometry();
		canvas_tiles.push_back({
			tile->renderKey().toStdString(),
			geometry.x(), geometry.y(), geometry.width(), geometry.height(),
			tile->isSpeaking(), tile->audioLevel(), tile->isVideoActive() && !tile->isVideoStreamPaused()
		});
		QPointer<VideoTileWidget> guarded(tile);
		_videoCanvas->setTileDecoration(tile->renderKey().toStdString(),
			[guarded](const QSize &pixels, bool hasFrame, bool hovered) {
				return guarded ? guarded->hardwareDecoration(pixels, hasFrame, hovered) : QImage();
			}, tile->pinButtonRect());
		tile->setHardwareCanvasMode(true);
		tile->hide();
	}

	_videoCanvas->setGeometry(_stageContainer->rect());
	_videoCanvas->setTilesLayout(canvas_tiles);
	_videoCanvas->show();
	_videoCanvas->raise();
	if (_videoPagingControls && _videoPagingControls->isVisible()) {
		_videoPagingControls->raise();
	}
	if (_inviteHintBanner) _inviteHintBanner->hide();
	if (_recoveryBanner && _recoveryBanner->isVisible()) {
		_recoveryBanner->raise();
	}
}

void MeetingRoomWindow::setupVideoCanvasInteractions() {
    _videoCanvas->setStageOverlay(_recoveryBanner);
    connect(_videoCanvas, &livekit::render::VideoCanvas::rendererInitialized,
        this, &MeetingRoomWindow::tryActivateGpuBackend,
        static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection));
	connect(_videoCanvas, &livekit::render::VideoCanvas::tileDoubleClicked,
		this, &MeetingRoomWindow::togglePinForRenderKey, Qt::UniqueConnection);
	connect(_videoCanvas, &livekit::render::VideoCanvas::tilePinRequested,
		this, &MeetingRoomWindow::togglePinForRenderKey, Qt::UniqueConnection);
}

void MeetingRoomWindow::bindTileInteractions(VideoTileWidget *tile) {
	connect(tile, &VideoTileWidget::tileDoubleClicked, this, [this, tile] {
		setPinnedTile(tile->renderKey(), !tile->isPinned());
	});
	connect(tile, &VideoTileWidget::pinToggled, this, [this, tile](bool pinned) {
		setPinnedTile(tile->renderKey(), pinned);
	});
	connect(tile, &VideoTileWidget::presentationChanged, this, [this, tile] {
		if (_videoCanvas) _videoCanvas->updateTilePresentation(tile->renderKey().toStdString(),
			tile->isVideoActive() && !tile->isVideoStreamPaused());
	});
}

void MeetingRoomWindow::setPinnedTile(const QString &renderKey, bool pinned) {
	if (pinned) {
		_pinnedRenderKey = renderKey;
		_pinnedTrackKey = trackKeyForRenderKey(renderKey);
	} else if (_pinnedRenderKey == renderKey) {
		_pinnedRenderKey.clear();
		_pinnedTrackKey.reset();
	}
	scheduleViewportIntent(true);
}

void MeetingRoomWindow::togglePinForRenderKey(const QString &renderKey) {
	const auto toggle = [&](VideoTileWidget *tile) {
		if (!tile || tile->renderKey() != renderKey) return false;
		setPinnedTile(tile->renderKey(), _pinnedRenderKey != tile->renderKey());
		return true;
	};
	if (toggle(_localTile) || toggle(_localScreenTile.get())) return;
	for (const auto &[id, tile] : _remoteTiles) if (toggle(tile.get())) return;
	for (const auto &[sid, tile] : _remoteScreenTiles) if (toggle(tile.get())) return;
}

void MeetingRoomWindow::setupWhiteboardBinding() {
	_bottomBar->whiteboardClicked() | rpl::on_next([this] {
		setWhiteboardVisible(!_whiteboardVisible);
	}, lifetime());
}

void MeetingRoomWindow::setupRemoteControl() {
    if (_remoteControlUi || !_coordinator || !_stageContainer) return;
    _remoteControlUi = std::make_unique<RemoteControlUi>(_stageContainer,_coordinator.get(),[this] {
        std::vector<RemoteControlTarget> targets;
        if (_whiteboardVisible || !_coordinator || _coordinator->state() != OpenMeeting::MeetingState::InMeeting) return targets;
        for (const auto& [sid,binding] : _remoteVideoBindings) {
            if (!binding.screen || !canRenderRemoteVideo(sid)) continue;
            auto* tile = remoteVideoTile(sid);
            if (!tile) continue;
            QWidget* widget = tile;
            QRectF content;
            if (_usingGpuBackend.load() && _videoCanvas && _videoCanvas->isVisible()) {
                widget = _videoCanvas;
                content = _videoCanvas->videoContentRect(tile->renderKey().toStdString());
            } else if (tile->isVisible()) content = tile->remoteControlContentRect();
            targets.push_back({binding.identity,sid,tile->displayName(),widget,content});
        }
        return targets;
    },[this](bool blocked) {
        if (_annotationOverlay) _annotationOverlay->setInteractionEnabled(!blocked && _coordinator &&
            _coordinator->state() == OpenMeeting::MeetingState::InMeeting);
        if (_annotationButton) _annotationButton->setEnabled(!blocked && _annotationBinding.has_value());
    },[this](const QString& status) { updateRemoteControlStatus(status); });
    connect(_coordinator.get(),&OpenMeeting::MeetingCoordinator::remoteControlChanged,this,
        [this](const livekit::remote_control::Projection& p) {
            std::string key;
            if (p.state == livekit::remote_control::State::Controlling)
                if (auto* tile = remoteVideoTile(QString::fromStdString(p.track))) key = tile->renderKey().toStdString();
            if (_videoCanvas) _videoCanvas->setRemoteInputKey(std::move(key));
            for (const auto& [sid,tile] : _remoteScreenTiles) tile->setRemoteInputEnabled(
                p.state == livekit::remote_control::State::Controlling && sid.toStdString() == p.track);
        });
}

void MeetingRoomWindow::setupVideoPagingControls() {
	if (_videoPagingControls || !_stageContainer) return;
	_videoPagingControls = new QWidget(_stageContainer);
	// This small overlay must stack above the native GPU surface. Keep its
	// ancestors/siblings in the raster backing store and track stage movement.
	new NativeChildGeometry(*_videoPagingControls);
	_videoPagingControls->setAttribute(Qt::WA_NativeWindow);
	_videoPagingControls->setObjectName(QStringLiteral("videoPagingControls"));
	AppTheme::setTone(*_videoPagingControls, AppTheme::Tone::Dark);
	auto *layout = new QHBoxLayout(_videoPagingControls);
	layout->setContentsMargins(8, 6, 8, 6);
	layout->setSpacing(6);

	_previousVideoPage = new QToolButton(_videoPagingControls);
	_previousVideoPage->setObjectName(QStringLiteral("previousVideoPage"));
	_previousVideoPage->setAutoRaise(true);
	_previousVideoPage->setArrowType(Qt::LeftArrow);
	_previousVideoPage->setToolTip(
		QCoreApplication::translate("MeetingUI", "Previous video page"));
	_previousVideoPage->setAccessibleName(_previousVideoPage->toolTip());

	_videoPageLabel = new QLabel(QStringLiteral("1 / 1"), _videoPagingControls);
	_videoPageLabel->setObjectName(QStringLiteral("videoPageIndicator"));
	_videoPageLabel->setAlignment(Qt::AlignCenter);
	_videoPageLabel->setMinimumWidth(54);

	_nextVideoPage = new QToolButton(_videoPagingControls);
	_nextVideoPage->setObjectName(QStringLiteral("nextVideoPage"));
	_nextVideoPage->setAutoRaise(true);
	_nextVideoPage->setArrowType(Qt::RightArrow);
	_nextVideoPage->setToolTip(
		QCoreApplication::translate("MeetingUI", "Next video page"));
	_nextVideoPage->setAccessibleName(_nextVideoPage->toolTip());

	_videoPageSizeControl = createAccessibleComboBox(_videoPagingControls);
	_videoPageSizeControl->setObjectName(QStringLiteral("videoPageSize"));
	_videoPageSizeControl->setToolTip(
		QCoreApplication::translate("MeetingUI", "Videos per page"));
	_videoPageSizeControl->setAccessibleName(_videoPageSizeControl->toolTip());
	for (const auto size : {4, 9, 16}) {
		_videoPageSizeControl->addItem(QString::number(size), size);
	}
	_videoPageSizeControl->setCurrentIndex(
		_videoPageSizeControl->findData(static_cast<int>(_videoPageSize)));
	AppTheme::styleChoiceControls(*_videoPagingControls, AppTheme::Tone::Dark);

	layout->addWidget(_previousVideoPage);
	layout->addWidget(_videoPageLabel);
	layout->addWidget(_nextVideoPage);
	layout->addWidget(_videoPageSizeControl);
	_videoPagingControls->adjustSize();
	_videoPagingControls->hide();

	connect(_previousVideoPage, &QToolButton::clicked, this, [this] {
		if (_videoPage == 0) return;
		--_videoPage;
		scheduleViewportIntent(true);
	});
	connect(_nextVideoPage, &QToolButton::clicked, this, [this] {
		if (_videoPage + 1 >= _acceptedVideoPlan.page_count) return;
		++_videoPage;
		scheduleViewportIntent(true);
	});
	connect(_videoPageSizeControl, qOverload<int>(&QComboBox::currentIndexChanged),
		this, [this](int index) {
			if (!_videoPageSizeControl || index < 0) return;
			const auto requested = static_cast<uint32_t>(
				_videoPageSizeControl->itemData(index).toUInt());
			if (requested == _videoPageSize) return;
			_videoPageSize = requested;
			_videoPage = 0;
			scheduleViewportIntent(true);
		});

	_viewportIntentTimer = new QTimer(this);
	_viewportIntentTimer->setSingleShot(true);
	connect(_viewportIntentTimer, &QTimer::timeout,
		this, &MeetingRoomWindow::submitViewportIntent);
}

void MeetingRoomWindow::scheduleViewportIntent(bool immediate) {
	if (!_viewportIntentTimer) return;
	if (immediate) {
		_viewportIntentTimer->stop();
		submitViewportIntent();
	} else {
		_viewportIntentTimer->start(200);
	}
}

void MeetingRoomWindow::submitViewportIntent() {
	if (!_coordinator || !_stageContainer || _closeRequested) return;
	livekit::ViewportIntent intent;
	switch (_viewMode) {
	case VideoViewMode::Auto: intent.mode = livekit::VideoLayoutMode::Auto; break;
	case VideoViewMode::Grid: intent.mode = livekit::VideoLayoutMode::Grid; break;
	case VideoViewMode::Pip: intent.mode = livekit::VideoLayoutMode::PictureInPicture; break;
	case VideoViewMode::Speaker: intent.mode = livekit::VideoLayoutMode::Speaker; break;
	}
	intent.page = _videoPage;
	intent.page_size = _videoPageSize;
	intent.pinned = _pinnedTrackKey;
	intent.stage_content = _whiteboardVisible
		? livekit::StageContent::Whiteboard
		: livekit::StageContent::Video;
	intent.stage_rect = {0, 0, _stageContainer->width(), _stageContainer->height()};
	intent.device_pixel_ratio = devicePixelRatioF();
	intent.window_visible = _logicalWindowVisible;
	intent.minimized = isMinimized();
	if (_lastViewportIntent &&
		SameViewportIntentContent(*_lastViewportIntent, intent)) {
		return;
	}
	intent.view_revision = ++_viewportRevision;
	if (intent.view_revision == 0) intent.view_revision = ++_viewportRevision;
	_lastViewportIntent = intent;
	_coordinator->submitViewportIntent(std::move(intent));
}

void MeetingRoomWindow::setWhiteboardVisible(bool visible) {
	if (visible && !_whiteboardPanel) {
		_whiteboardPanel = new WhiteboardPanel(_stageContainer);
		connect(_whiteboardPanel, &WhiteboardPanel::closeRequested, this, [this] {
			setWhiteboardVisible(false);
		});
		if (_coordinator) {
			connect(_whiteboardPanel, &WhiteboardPanel::commandProposed,
				_coordinator.get(), &OpenMeeting::MeetingCoordinator::submitWhiteboardCommand);
			connect(_whiteboardPanel, &WhiteboardPanel::imageProposed,
				_coordinator.get(), &OpenMeeting::MeetingCoordinator::submitWhiteboardImage);
			connect(_whiteboardPanel, &WhiteboardPanel::lockRequested,
				_coordinator.get(), &OpenMeeting::MeetingCoordinator::setWhiteboardLocked);
			connect(_whiteboardPanel, &WhiteboardPanel::writersOpenRequested,
				_coordinator.get(), &OpenMeeting::MeetingCoordinator::setWhiteboardWritersOpen);
			connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::whiteboardProjectionChanged,
				_whiteboardPanel, &WhiteboardPanel::applyProjection);
			if (_coordinator->state() == OpenMeeting::MeetingState::InMeeting ||
				_coordinator->state() == OpenMeeting::MeetingState::Reconnecting) {
				_whiteboardPanel->enableCollaboration();
				_coordinator->activateWhiteboard();
			}
		}
	}
	_whiteboardVisible = visible;
	if (!visible && _whiteboardPanel) _whiteboardPanel->hide();
	scheduleViewportIntent(true);
	updateVideoLayout();
	if (visible) _whiteboardPanel->canvas()->setFocus(Qt::OtherFocusReason);
	else tryActivateGpuBackend();
}

bool MeetingRoomWindow::isVideoStageVisible() const {
	return _logicalWindowVisible && !isMinimized() && !_whiteboardVisible &&
		_stageContainer && _stageContainer->width() > 0 &&
		_stageContainer->height() > 0;
}

void MeetingRoomWindow::updateVideoLayout() {
    if (_remoteRenderSession && _localVideoSource) {
        if (_config.videoEnabled && isVideoStageVisible()) {
            _remoteRenderSession->AttachLocalSource(_localVideoSource);
        } else {
            _remoteRenderSession->DetachLocalSource();
            if (_videoCanvas) _videoCanvas->removeUser("local");
            if (_localTile) _localTile->setFrame({});
        }
    }
	const int stageW = _stageContainer->width();
	const int stageH = _stageContainer->height();
	if (stageW <= 0 || stageH <= 0) return;

	std::vector<VideoTileWidget*> allTiles;
	if (_localTile && isVideoStageVisible()) {
		allTiles.push_back(_localTile);
	}
	for (const auto &seat : _acceptedVideoPlan.visible_seats) {
		if (auto *tile = remoteVideoTile(seat.key);
			tile && std::find(allTiles.begin(), allTiles.end(), tile) == allTiles.end()) {
			allTiles.push_back(tile);
		}
	}
	if (_localScreenTile && isVideoStageVisible()) allTiles.push_back(_localScreenTile.get());
	if (!isVideoStageVisible()) {
		for (auto *tile : allTiles) if (tile) tile->hide();
		for (auto &[_, tile] : _remoteTiles) if (tile) tile->hide();
		for (auto &[_, tile] : _remoteScreenTiles) if (tile) tile->hide();
		if (_localTile) _localTile->hide();
		if (_localScreenTile) _localScreenTile->hide();
		if (_videoCanvas) _videoCanvas->hide();
		if (_inviteHintBanner) _inviteHintBanner->hide();
		if (_videoPagingControls) _videoPagingControls->hide();
		if (_whiteboardVisible && _whiteboardPanel) {
			_whiteboardPanel->setGeometry(_stageContainer->rect());
			_whiteboardPanel->show();
			_whiteboardPanel->raise();
		}
		if (_recoveryBanner && _recoveryBanner->isVisible()) {
			const auto size = bannerSize(*_recoveryBanner, stageW, 420);
			_recoveryBanner->setGeometry((stageW - size.width()) / 2, 16, size.width(), size.height());
			_recoveryBanner->raise();
		}
		return;
	}
	if (_whiteboardPanel) _whiteboardPanel->hide();
	if (_videoPagingControls) {
		_videoPagingControls->adjustSize();
		_videoPagingControls->move(
			std::max(8, stageW - _videoPagingControls->width() - 12),
			std::max(8, stageH - _videoPagingControls->height() - 12));
		_videoPagingControls->setVisible(_acceptedVideoPlan.page_count > 1);
		_videoPagingControls->raise();
	}
	if (std::none_of(allTiles.begin(), allTiles.end(), [this](const auto *tile) {
		return tile->renderKey() == _pinnedRenderKey;
	})) _pinnedRenderKey.clear();
	for (auto *tile : allTiles) tile->setPinned(tile->renderKey() == _pinnedRenderKey);
	if (!_usingGpuBackend.load(std::memory_order_acquire)) {
		for (auto *tile : allTiles) tile->setHardwareCanvasMode(false);
	}

	const int N = static_cast<int>(allTiles.size());
	if (N == 0) return;

	const bool hasRemote = !_remoteTiles.empty();
	const bool localActive = _localTile && _localTile->isVideoActive();

 const auto inviteSize = bannerSize(*_inviteHintBanner, stageW, 220);
 const int bannerW = inviteSize.width();
 const int bannerH = inviteSize.height();
	_inviteHintBanner->setGeometry((stageW - bannerW) / 2, stageH - bannerH - 12, bannerW, bannerH);
	_inviteHintBanner->setVisible(!_usingGpuBackend.load(std::memory_order_acquire) && !hasRemote && !localActive);

	if (_recoveryBanner && _recoveryBanner->isVisible()) {
  const auto recoverySize = bannerSize(*_recoveryBanner, stageW, 420);
  const int recBannerW = recoverySize.width();
  const int recBannerH = recoverySize.height();
		_recoveryBanner->setGeometry((stageW - recBannerW) / 2, 16, recBannerW, recBannerH);
		_recoveryBanner->raise();
	}

	// 1. 画中画模式 (PiP)
	if (_acceptedVideoPlan.mode == livekit::VideoLayoutMode::PictureInPicture && N >= 2) {
		VideoTileWidget *mainTile = allTiles[1];
		if (_acceptedVideoPlan.focused) {
			if (auto *focused = remoteVideoTile(*_acceptedVideoPlan.focused)) mainTile = focused;
		}
		// One back-to-front order drives both QWidget stacking and DX11 drawing.
		const auto main = std::find(allTiles.begin(), allTiles.end(), mainTile);
		std::rotate(allTiles.begin(), main, main + 1);

		mainTile->setPipMode(false);
		mainTile->setGeometry(0, 0, stageW, stageH);
		mainTile->show();
		mainTile->lower();

		const int pipW = std::clamp(stageW * 22 / 100, 160, 260);
		const int pipH = pipW * 9 / 16;
		int pipRightOffset = 16;

		for (auto *t : allTiles) {
			if (t == mainTile) continue;
			t->setPipMode(true);
			t->setGeometry(stageW - pipW - pipRightOffset, stageH - pipH - 16, pipW, pipH);
			t->show();
			t->raise();
			pipRightOffset += pipW + 10;
		}
		syncVideoCanvasLayout(allTiles);
		return;
	}

	// 2. 演讲者聚焦模式 (Speaker / Focus Mode)
	if (_acceptedVideoPlan.mode == livekit::VideoLayoutMode::Speaker && N >= 2) {
		VideoTileWidget *focusTile = allTiles[0];
		if (_acceptedVideoPlan.focused) {
			if (auto *focused = remoteVideoTile(*_acceptedVideoPlan.focused)) {
				focusTile = focused;
			}
		} else if (focusTile == _localTile && allTiles.size() > 1) {
			focusTile = allTiles[1];
		}

		std::vector<VideoTileWidget*> otherTiles;
		for (auto *t : allTiles) {
			if (t != focusTile) otherTiles.push_back(t);
		}

		const int margin = 8;
		const int gap = 8;
		const int filmstripW = std::clamp(stageW * 24 / 100, 180, 260);
		const int mainW = stageW - filmstripW - gap - margin * 2;
		const int mainH = stageH - margin * 2;

		focusTile->setPipMode(false);
		focusTile->setGeometry(margin, margin, mainW, mainH);
		focusTile->show();

		const int numOthers = static_cast<int>(otherTiles.size());
		const int smallH = (mainH - (numOthers - 1) * gap) / std::max(1, numOthers);
		const int clampedH = std::clamp(smallH, 90, filmstripW * 9 / 16);

		for (int i = 0; i < numOthers; ++i) {
			otherTiles[i]->setPipMode(false);
			otherTiles[i]->setGeometry(margin + mainW + gap, margin + i * (clampedH + gap), filmstripW, clampedH);
			otherTiles[i]->show();
		}
		syncVideoCanvasLayout(allTiles);
		return;
	}

	// 3. 自适应 16:9 最优画廊宫格模式 (Optimal Gallery Grid)
	const int margin = 8;
	const int gap = 8;
	int bestRows = 1, bestCols = 1;
	int bestTileW = 0, bestTileH = 0;
	double maxArea = 0.0;

	for (int cols = 1; cols <= N; ++cols) {
		int rows = (N + cols - 1) / cols;
		int availW = stageW - margin * 2 - (cols - 1) * gap;
		int availH = stageH - margin * 2 - (rows - 1) * gap;
		if (availW <= 0 || availH <= 0) continue;

		int maxW = availW / cols;
		int maxH = availH / rows;

		int tW = maxW;
		int tH = maxH;
		if (static_cast<double>(maxW) / maxH > 16.0 / 9.0) {
			tW = static_cast<int>(maxH * 16.0 / 9.0);
			tH = maxH;
		} else {
			tW = maxW;
			tH = static_cast<int>(maxW * 9.0 / 16.0);
		}

		double area = static_cast<double>(tW) * tH;
		if (area > maxArea) {
			maxArea = area;
			bestRows = rows;
			bestCols = cols;
			bestTileW = tW;
			bestTileH = tH;
		}
	}

	const int totalGridH = bestRows * bestTileH + (bestRows - 1) * gap;
	const int startY = (stageH - totalGridH) / 2;

	int tileIdx = 0;
	for (int r = 0; r < bestRows && tileIdx < N; ++r) {
		int itemsInRow = std::min(bestCols, N - r * bestCols);
		int rowW = itemsInRow * bestTileW + (itemsInRow - 1) * gap;
		int startX = (stageW - rowW) / 2;

		for (int c = 0; c < itemsInRow && tileIdx < N; ++c) {
			QRect geom(startX + c * (bestTileW + gap), startY + r * (bestTileH + gap), bestTileW, bestTileH);
			allTiles[tileIdx]->setPipMode(false);
			allTiles[tileIdx]->setGeometry(geom);
			allTiles[tileIdx]->show();
			++tileIdx;
		}
	}
	syncVideoCanvasLayout(allTiles);
}

void MeetingRoomWindow::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.fillRect(rect(), QColor(0x12, 0x14, 0x1a));
}

void MeetingRoomWindow::receiveRenderedVideoFrame(
		const QImage& image,
		const QString& key,
		livekit::render::VideoRenderFrame::Ptr renderFrame) {
    if (_remoteControlUi) _remoteControlUi->noteFrame(key);
    if (key == QStringLiteral("local")) {
        if (_config.videoEnabled) receiveLocalVideoFrame(image, std::move(renderFrame));
    } else if (_localScreenTile && key == _localScreenTile->renderKey()) {
        _localScreenTile->setFrame(image, std::move(renderFrame));
    } else {
        receiveRemoteVideoFrame(image, key, std::move(renderFrame));
    }
}

void MeetingRoomWindow::receiveGpuVideoFrame(const std::string& key, livekit::render::VideoRenderFrame::Ptr frame) {
    if (_remoteControlUi) _remoteControlUi->noteFrame(QString::fromStdString(key));
    if (!_usingGpuBackend.load(std::memory_order_acquire) || !_videoCanvas) return;
    const auto qkey = QString::fromStdString(key);
    VideoTileWidget* tile = nullptr;
    if (qkey == QStringLiteral("local")) {
        if (!_config.videoEnabled) return;
        tile = _localTile;
    } else if (_localScreenTile && qkey == _localScreenTile->renderKey()) {
        tile = _localScreenTile.get();
    } else {
        if (!canRenderRemoteVideo(qkey)) return;
        tile = remoteVideoTile(qkey);
        if (tile && !tile->isVideoActive()) { tile->setVideoActive(true); updateVideoLayout(); }
    }
    if (tile) _videoCanvas->updateFrame(tile->renderKey().toStdString(), std::move(frame));
}

void MeetingRoomWindow::receiveRemoteVideoFrame(
		const QImage &frame,
		const QString &trackSid,
		livekit::render::VideoRenderFrame::Ptr renderFrame) {
	if (frame.isNull() || !canRenderRemoteVideo(trackSid)) return;
	if (auto *tile = remoteVideoTile(trackSid)) {
		tile->setFrame(frame, std::move(renderFrame));
		if (!tile->isVideoActive()) {
			tile->setVideoActive(true);
			updateVideoLayout();
		}
	}
}

void MeetingRoomWindow::receiveLocalVideoFrame(
		const QImage &frame,
		livekit::render::VideoRenderFrame::Ptr renderFrame) {
	if (_localTile) {
		_localTile->setFrame(frame, std::move(renderFrame));
	}
}



void MeetingRoomWindow::onLocalVideoGenerated() {
	if (!_localVideoSource) return;

	_localFrameStep++;
	const int w = 1280;
	const int h = 720;

	livekit::VideoFrame frame = livekit::VideoFrame::create(w, h, livekit::VideoBufferType::RGBA);
	uint8_t *data = frame.data();
	if (!data) return;

	const int shift = (_localFrameStep * 4) % w;
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			int idx = (y * w + x) * 4;
			int col_sec = ((x + shift) * 7) / w;
			uint8_t r = 0, g = 0, b = 0;
			switch (col_sec % 7) {
			case 0: r = 38; g = 110; b = 240; break;
			case 1: r = 0; g = 180; b = 136; break;
			case 2: r = 255; g = 125; b = 0; break;
			case 3: r = 245; g = 63; b = 63; break;
			case 4: r = 114; g = 46; b = 209; break;
			case 5: r = 22, g = 93, b = 255; break;
			case 6: r = 20, g = 201, b = 201; break;
			}
			if (y > h * 4 / 5) {
				int bar_x = (_localFrameStep * 8) % w;
				if (std::abs(x - bar_x) < 24) {
					r = 255; g = 255; b = 255;
				} else {
					r = (x * 200) / w;
					g = (y * 200) / h;
					b = 100;
				}
			}
			data[idx] = r;
			data[idx + 1] = g;
			data[idx + 2] = b;
			data[idx + 3] = 255;
		}
	}

	livekit::VideoCaptureOptions opts;
	opts.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	opts.rotation = livekit::VideoRotation::VIDEO_ROTATION_0;

	_localVideoSource->captureFrame(frame, opts);
}

void MeetingRoomWindow::applyParticipantPresentation(
		const OpenMeeting::ParticipantPresentation &presentation) {
	QPointer<MeetingRoomWindow> owner(this);
	QPointer<OpenMeeting::MeetingCoordinator> coordinator(_coordinator.get());
	const auto current = [&](const OpenMeeting::RemoteVideoTrackPresentation *track = nullptr) {
		return owner && coordinator && owner->_coordinator.get() == coordinator &&
			coordinator->isParticipantPresentationCurrent(presentation, track);
	};
	if (presentation.participant.isLocal || !current()) return;
	applyRemoteParticipantJoined(presentation.participant.identity,
		presentation.participant.name, &presentation);
	if (!current()) return;
	for (const auto &track : presentation.videoTracks) {
		if (!current()) return;
		if (!current(&track)) continue;
		owner->attachRemoteVideo(presentation, track);
	}
}

void MeetingRoomWindow::restoreParticipantPresentations() {
	QPointer<MeetingRoomWindow> owner(this);
	QPointer<OpenMeeting::MeetingCoordinator> coordinator(_coordinator.get());
	if (!coordinator) return;
	const auto presentations = coordinator->participantPresentations();
	for (const auto &presentation : presentations) {
		if (!owner || !coordinator || owner->_coordinator.get() != coordinator ||
			coordinator->state() != OpenMeeting::MeetingState::InMeeting) return;
		owner->applyParticipantPresentation(presentation);
	}
}

void MeetingRoomWindow::requestScreenShare() {
	if (!_coordinator) return;
	using State = livekit::ScreenShareState;
	const auto state = _coordinator->screenShareSnapshot().state;
	if (state == State::Starting || state == State::Active || state == State::StopFailed) {
		_coordinator->stopScreenShare();
	} else if (state == State::Idle || state == State::Failed) {
		if (auto *picker = findChild<QDialog *>(QStringLiteral("screen-share-picker"))) {
			picker->raise();
			return;
		}
		_coordinator->requestScreenShareSources();
	}
}

void MeetingRoomWindow::requestDefaultScreenShare() {
	if (!_coordinator) return;
	using State = livekit::ScreenShareState;
	const auto state = _coordinator->screenShareSnapshot().state;
	if (state != State::Idle && state != State::Failed) return;
	_defaultScreenSharePending = true;
	_coordinator->requestScreenShareSources();
}

void MeetingRoomWindow::handleScreenShareSources(
		const std::vector<livekit::DesktopSource> &sources) {
	if (sources.empty()) {
		_defaultScreenSharePending = false;
		QMessageBox::warning(this, QCoreApplication::translate("MeetingUI", "Screen Sharing"),
			QCoreApplication::translate("MeetingUI", "No screens or windows available to share"));
		return;
	}
	if (_defaultScreenSharePending) {
		_defaultScreenSharePending = false;
		auto source = std::find_if(sources.begin(), sources.end(), [](const auto &candidate) {
			return candidate.kind == livekit::DesktopSourceKind::Screen;
		});
		if (source == sources.end()) source = sources.begin();
		if (_coordinator) _coordinator->startScreenShare(*source);
		return;
	}

	auto *dialog = new QDialog(this);
	dialog->setObjectName(QStringLiteral("screen-share-picker"));
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	dialog->setWindowTitle(QCoreApplication::translate("MeetingUI", "Select a Source to Share"));
	auto *layout = new QVBoxLayout(dialog);
	auto *sourceLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Select a screen or window (video only):"), dialog);
	layout->addWidget(sourceLabel);
	auto *sourceCombo = createAccessibleComboBox(dialog);
	sourceCombo->setObjectName(QStringLiteral("screenShareSource"));
	sourceCombo->setAccessibleName(QCoreApplication::translate("MeetingUI", "Share source"));
	sourceLabel->setBuddy(sourceCombo);
	for (size_t i = 0; i < sources.size(); ++i) {
		const auto &source = sources[i];
		const auto kind = source.kind == livekit::DesktopSourceKind::Screen
			? QCoreApplication::translate("MeetingUI", "Screen") : QCoreApplication::translate("MeetingUI", "Window");
		sourceCombo->addItem(QString::number(i + 1) + QStringLiteral(". ") + kind +
			QStringLiteral(" — ") + QString::fromStdString(source.title));
	}
	layout->addWidget(sourceCombo);
	auto *fpsLabel = new QLabel(QCoreApplication::translate("MeetingUI", "Screen share frame rate"), dialog);
	layout->addWidget(fpsLabel);
	auto *fpsCombo = createAccessibleComboBox(dialog);
	fpsCombo->setObjectName(QStringLiteral("screenShareStartFps"));
	fpsCombo->setAccessibleName(fpsLabel->text());
	fpsLabel->setBuddy(fpsCombo);
	for (int fps : {15, 20, 30}) fpsCombo->addItem(QString::number(fps) + QStringLiteral(" FPS"), fps);
	const int preferredFps = OpenMeeting::SessionManager::instance().mediaPreferences().screenShareFps;
	const int preferredIndex = fpsCombo->findData(preferredFps);
	fpsCombo->setCurrentIndex(preferredIndex >= 0 ? preferredIndex : fpsCombo->findData(20));
	layout->addWidget(fpsCombo);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
	layout->addWidget(buttons);
	buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("screenShareAccept"));
	buttons->button(QDialogButtonBox::Cancel)->setObjectName(QStringLiteral("screenShareCancel"));
	connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
	QPointer<OpenMeeting::MeetingCoordinator> coordinator(_coordinator.get());
	const std::weak_ptr<livekit::Room> room = _coordinator->room();
	connect(dialog, &QDialog::accepted, this,
		[coordinator, room, sources, sourceCombo, fpsCombo] {
			const int index = sourceCombo->currentIndex();
			if (coordinator && !room.expired() && coordinator->room() == room.lock() &&
				index >= 0 && index < static_cast<int>(sources.size()))
				coordinator->startScreenShare(sources[index], fpsCombo->currentData().toInt());
		});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::stateChanged,
		dialog, [dialog](OpenMeeting::MeetingState state, const QString &) {
			if (state != OpenMeeting::MeetingState::InMeeting) dialog->reject();
		});
	dialog->open();
}

bool MeetingRoomWindow::isTrackVisible(const livekit::TrackKey &key) const {
	return std::any_of(
		_acceptedVideoPlan.visible_seats.begin(),
		_acceptedVideoPlan.visible_seats.end(),
		[&](const auto &seat) { return seat.key == key; });
}

std::optional<livekit::TrackKey> MeetingRoomWindow::trackKeyForRenderKey(
		const QString &renderKey) const {
	if (renderKey.isEmpty()) return std::nullopt;
	for (const auto &seat : _acceptedVideoPlan.visible_seats) {
		const auto *tile = remoteVideoTile(seat.key);
		if (tile && tile->renderKey() == renderKey) return seat.key;
	}
	return std::nullopt;
}

VideoTileWidget *MeetingRoomWindow::ensureRemoteCameraTile(
		const QString &identity,
		const QString &name) {
	applyRemoteParticipantJoined(identity, name, nullptr);
	const auto found = _remoteTiles.find(identity);
	return found == _remoteTiles.end() ? nullptr : found->second.get();
}

VideoTileWidget *MeetingRoomWindow::ensureRemoteScreenTile(
		const livekit::VideoSeat &seat,
		const QString &name) {
	const auto sid = QString::fromStdString(seat.key.publication_sid);
	if (sid.isEmpty()) return nullptr;
	const auto existing = _remoteScreenTiles.find(sid);
	if (existing != _remoteScreenTiles.end()) return existing->second.get();
	auto tile = std::make_unique<VideoTileWidget>(
		QCoreApplication::translate("MeetingUI", "%1 · Screen Share").arg(name),
		false, _stageContainer, true);
	tile->setIdentity(QStringLiteral("remote-screen/") + sid);
	bindTileInteractions(tile.get());
	auto *result = tile.get();
	_remoteScreenTiles.emplace(sid, std::move(tile));
	return result;
}

VideoTileWidget *MeetingRoomWindow::remoteVideoTile(
		const livekit::TrackKey &key) const {
	const auto sid = QString::fromStdString(key.publication_sid);
	const auto binding = _remoteVideoBindings.find(sid);
	if (binding != _remoteVideoBindings.end()) return remoteVideoTile(sid);
	const auto seat = std::find_if(
		_acceptedVideoPlan.visible_seats.begin(),
		_acceptedVideoPlan.visible_seats.end(),
		[&](const auto &value) { return value.key == key; });
	if (seat == _acceptedVideoPlan.visible_seats.end()) return nullptr;
	if (seat->source == livekit::TrackSource::ScreenShareVideo) {
		const auto tile = _remoteScreenTiles.find(sid);
		return tile == _remoteScreenTiles.end() ? nullptr : tile->second.get();
	}
	const auto identity = QString::fromStdString(
		key.participant.identity.empty() ? key.participant.sid : key.participant.identity);
	const auto tile = _remoteTiles.find(identity);
	return tile == _remoteTiles.end() ? nullptr : tile->second.get();
}

void MeetingRoomWindow::applyAcceptedVideoDemandPlan(
		livekit::VideoDemandPlan plan) {
	if (_acceptedVideoPlan.coordinator_session == plan.coordinator_session &&
		plan.policy_revision < _acceptedVideoPlan.policy_revision) {
		return;
	}
	if (_acceptedVideoPlan.coordinator_session != 0 &&
		_acceptedVideoPlan.coordinator_session != plan.coordinator_session) {
		_activeRemoteRenderLeases.clear();
		_lastViewportIntent.reset();
	}
	_acceptedVideoPlan = std::move(plan);
	if (_acceptedVideoPlan.stage_content == livekit::StageContent::Video &&
		_acceptedVideoPlan.reason != livekit::VideoDemandReason::Hidden &&
		_pinnedTrackKey &&
		std::none_of(
			_acceptedVideoPlan.visible_seats.begin(),
			_acceptedVideoPlan.visible_seats.end(),
			[this](const auto &seat) { return seat.key == *_pinnedTrackKey; })) {
		_pinnedTrackKey.reset();
		_pinnedRenderKey.clear();
	}
	if (_acceptedVideoPlan.stage_content == livekit::StageContent::Video &&
		_acceptedVideoPlan.reason != livekit::VideoDemandReason::Hidden) {
		_videoPage = _acceptedVideoPlan.page;
	}
	if (_acceptedVideoPlan.mode == livekit::VideoLayoutMode::Grid &&
		(_acceptedVideoPlan.page_size == 4 ||
		 _acceptedVideoPlan.page_size == 9 ||
		 _acceptedVideoPlan.page_size == 16)) {
		_videoPageSize = _acceptedVideoPlan.page_size;
	}
	if (_videoPageSizeControl) {
		const QSignalBlocker blocker(_videoPageSizeControl);
		const auto index = _videoPageSizeControl->findData(
			static_cast<int>(_videoPageSize));
		if (index >= 0) _videoPageSizeControl->setCurrentIndex(index);
	}
	if (_videoPageLabel) {
		_videoPageLabel->setText(QStringLiteral("%1 / %2")
			.arg(_acceptedVideoPlan.page + 1)
			.arg(std::max<uint32_t>(1, _acceptedVideoPlan.page_count)));
	}
	if (_previousVideoPage) _previousVideoPage->setEnabled(_videoPage > 0);
	if (_nextVideoPage) {
		_nextVideoPage->setEnabled(
			_videoPage + 1 < _acceptedVideoPlan.page_count);
	}
	if (_videoPagingControls) {
		_videoPagingControls->setVisible(
			isVideoStageVisible() && _acceptedVideoPlan.page_count > 1);
	}
	syncVisibleRemoteTiles();
	reconcileRemoteRenderSelection();
	updateVideoLayout();
}

void MeetingRoomWindow::syncVisibleRemoteTiles() {
	std::set<QString> cameraIdentities;
	std::set<QString> screenSids;
	for (const auto &seat : _acceptedVideoPlan.visible_seats) {
		if (cameraIdentities.size() + screenSids.size() >= 16) break;
		const auto identity = QString::fromStdString(
			seat.key.participant.identity.empty()
				? seat.key.participant.sid
				: seat.key.participant.identity);
		const auto knownName = _remoteParticipantNames.find(identity);
		const auto name = knownName == _remoteParticipantNames.end() || knownName->second.isEmpty()
			? identity : knownName->second;
		if (seat.source == livekit::TrackSource::ScreenShareVideo) {
			const auto sid = QString::fromStdString(seat.key.publication_sid);
			screenSids.insert(sid);
			if (auto *tile = ensureRemoteScreenTile(seat, name)) {
				tile->setVideoSubscriptionError(seat.subscription_error);
			}
		} else {
			cameraIdentities.insert(identity);
			auto *tile = ensureRemoteCameraTile(identity, name);
			if (tile) tile->setVideoSubscriptionError(seat.subscription_error);
			if (tile && seat.IsParticipantPlaceholder()) {
				tile->setFrame({});
				tile->setVideoActive(false);
				tile->setVideoStreamPaused(false);
			}
		}
	}

	for (auto it = _remoteTiles.begin(); it != _remoteTiles.end();) {
		if (cameraIdentities.count(it->first)) {
			++it;
			continue;
		}
		if (it->second) {
			if (_videoCanvas) _videoCanvas->removeUser(
				it->second->renderKey().toStdString());
			it->second->setFrame({});
		}
		it = _remoteTiles.erase(it);
	}
	for (auto it = _remoteScreenTiles.begin(); it != _remoteScreenTiles.end();) {
		if (screenSids.count(it->first)) {
			++it;
			continue;
		}
		if (it->second) {
			if (_videoCanvas) _videoCanvas->removeUser(
				it->second->renderKey().toStdString());
			it->second->setFrame({});
		}
		it = _remoteScreenTiles.erase(it);
	}
}

void MeetingRoomWindow::reconcileRemoteRenderSelection() {
	if (!_remoteRenderSession) return;
	std::vector<livekit::render::VideoRenderSession::RemoteTrackSelection> selection;
	selection.reserve(_acceptedVideoPlan.selected_video.size());
	for (const auto &key : _acceptedVideoPlan.selected_video) {
		const auto sid = QString::fromStdString(key.publication_sid);
		const auto binding = _remoteVideoBindings.find(sid);
		if (binding == _remoteVideoBindings.end() || binding->second.key != key ||
			binding->second.muted || binding->second.paused ||
			remoteVideoSubscriptionError(key) != livekit::TrackPublication::SubscriptionError::None) {
			continue;
		}
		const auto track = binding->second.track.lock();
		if (!track ||
			!livekit::IsTrackTicketActive(binding->second.ticket, key) ||
			!livekit::IsMediaBindingTicketActive(
				binding->second.mediaBindingTicket,
				binding->second.mediaBindingKey)) {
			continue;
		}
		livekit::render::VideoRenderSession::RemoteTrackSelection value;
		value.key = key;
		value.ticket = binding->second.ticket;
		value.media_binding_key = binding->second.mediaBindingKey;
		value.media_binding_ticket = binding->second.mediaBindingTicket;
		value.track = std::move(track);
		value.identity = binding->second.identity.toStdString();
		value.render_key = key.publication_sid;
		selection.push_back(std::move(value));
	}
	const auto result = _remoteRenderSession->ApplySelection(
		_acceptedVideoPlan.coordinator_session,
		_acceptedVideoPlan.policy_revision,
		selection);
	livekit::VideoRenderSelectionObservation observation;
	observation.coordinator_session = _acceptedVideoPlan.coordinator_session;
	observation.catalog_revision = _acceptedVideoPlan.catalog_revision;
	observation.policy_revision = _acceptedVideoPlan.policy_revision;
	observation.actual_video.reserve(selection.size());
	observation.bound_video.reserve(result.attached);
	for (const auto &value : selection) {
		observation.actual_video.push_back(value.key);
	}
	std::map<QString, livekit::MediaBindingKey> active;
	for (std::size_t index = 0; index < result.tracks.size() &&
		index < selection.size(); ++index) {
		const auto status = result.tracks[index].second;
		if (status == livekit::render::VideoRenderSession::AttachResult::Attached ||
			status == livekit::render::VideoRenderSession::AttachResult::AlreadyBound) {
			active[QString::fromStdString(selection[index].key.publication_sid)] =
				selection[index].media_binding_key;
			observation.bound_video.push_back(selection[index].key);
		}
	}
	for (const auto &[sid, oldBinding] : _activeRemoteRenderLeases) {
		const auto current = active.find(sid);
		if (current != active.end() && current->second == oldBinding) continue;
		if (auto *tile = remoteVideoTile(sid)) {
			if (_videoCanvas) _videoCanvas->removeUser(
				tile->renderKey().toStdString());
			tile->setFrame({});
		}
	}
	_activeRemoteRenderLeases = std::move(active);
	if (_coordinator) {
		_coordinator->reportVideoRenderSelection(std::move(observation));
	}
}

VideoTileWidget *MeetingRoomWindow::remoteVideoTile(const QString &trackSid) const {
	const auto found = _remoteVideoBindings.find(trackSid);
	if (found == _remoteVideoBindings.end()) return nullptr;
	if (found->second.screen) {
		const auto tile = _remoteScreenTiles.find(trackSid);
		return tile == _remoteScreenTiles.end() ? nullptr : tile->second.get();
	}
	const auto tile = _remoteTiles.find(found->second.identity);
	return tile == _remoteTiles.end() ? nullptr : tile->second.get();
}

bool MeetingRoomWindow::canRenderRemoteVideo(const QString &trackSid) const {
	const auto found = _remoteVideoBindings.find(trackSid);
	if (found == _remoteVideoBindings.end()) return false;
	const auto &binding = found->second;
	const auto lease = _activeRemoteRenderLeases.find(trackSid);
	if (lease == _activeRemoteRenderLeases.end() ||
		lease->second != binding.mediaBindingKey ||
		!isTrackVisible(binding.key)) {
		return false;
	}
	const auto track = binding.track.lock();
	return track && !track->muted() && !binding.muted && !binding.paused &&
		remoteVideoSubscriptionError(binding.key) == livekit::TrackPublication::SubscriptionError::None &&
		livekit::IsMediaBindingTicketActive(binding.mediaBindingTicket, binding.mediaBindingKey);
}

livekit::TrackPublication::SubscriptionError MeetingRoomWindow::remoteVideoSubscriptionError(
		const livekit::TrackKey &key) const {
	for (const auto &seat : _acceptedVideoPlan.visible_seats) {
		if (seat.key == key) return seat.subscription_error;
	}
	return livekit::TrackPublication::SubscriptionError::None;
}

void MeetingRoomWindow::refreshRemoteVideoPresentations() {
	QPointer<MeetingRoomWindow> owner(this);
	QPointer<OpenMeeting::MeetingCoordinator> coordinator(_coordinator.get());
	if (!coordinator) return;
	const auto presentations = coordinator->participantPresentations();
	for (const auto &presentation : presentations) {
		for (const auto &video : presentation.videoTracks) {
			if (!owner || !coordinator || owner->_coordinator.get() != coordinator) return;
			owner->attachRemoteVideo(presentation, video);
		}
	}
}

void MeetingRoomWindow::attachRemoteVideo(const OpenMeeting::ParticipantPresentation &presentation,
		const OpenMeeting::RemoteVideoTrackPresentation &value) {
	if (!_remoteRenderSession || !_coordinator ||
		!_coordinator->isParticipantPresentationCurrent(presentation, &value)) return;
	const auto &identity = presentation.participant.identity;
	const auto sid = QString::fromStdString(value.key.publication_sid);
	const bool screen = value.track->source() == livekit::TrackSource::ScreenShareVideo;
	QPointer<MeetingRoomWindow> owner(this);
	QPointer<OpenMeeting::MeetingCoordinator> coordinator(_coordinator.get());
	const auto current = [&] {
		return owner && coordinator && owner->_coordinator.get() == coordinator &&
			coordinator->isParticipantPresentationCurrent(presentation, &value);
	};
	if (const auto old = _remoteVideoBindings.find(sid);
		old != _remoteVideoBindings.end() && old->second.screen != screen) removeRemoteVideo(sid);
	if (!current()) return;
	const auto old = _remoteVideoBindings.find(sid);
	const bool resetFrame = old == _remoteVideoBindings.end() ||
		old->second.identity != identity || old->second.track.lock() != value.track ||
		old->second.mediaBindingKey != value.mediaBindingKey ||
		old->second.muted != value.muted || old->second.paused != value.paused;
	_remoteVideoBindings[sid] = {identity, screen, value.track,
		value.key, value.ticket, value.mediaBindingKey, value.mediaBindingTicket,
		value.muted, value.paused};
	if (screen && isTrackVisible(value.key) && !_remoteScreenTiles.count(sid)) {
		const auto seat = std::find_if(
			_acceptedVideoPlan.visible_seats.begin(),
			_acceptedVideoPlan.visible_seats.end(),
			[&](const auto &candidate) { return candidate.key == value.key; });
		if (seat != _acceptedVideoPlan.visible_seats.end()) {
			ensureRemoteScreenTile(*seat, presentation.participant.name);
		}
	}
	if (!current()) return;
	QPointer<VideoTileWidget> tile(remoteVideoTile(sid));
	if (resetFrame) {
		// Revoke the render subscription before clearing either backend. This also
		// rejects callbacks already in flight when the same Track is rebound.
		_remoteRenderSession->RemoveTrack(sid.toStdString());
		_activeRemoteRenderLeases.erase(sid);
		if (tile) {
			if (_videoCanvas) _videoCanvas->removeUser(tile->renderKey().toStdString());
			tile->setFrame({});
		}
	}
	if (tile) {
		tile->setVideoActive(!value.muted);
		tile->setVideoStreamPaused(!value.muted && value.paused);
		tile->setVideoSubscriptionError(remoteVideoSubscriptionError(value.key));
		tile->setConnectionQuality(presentation.participant.connectionQuality);
		if (screen) tile->setDisplayName(QCoreApplication::translate("MeetingUI", "%1 · Screen Share").arg(presentation.participant.name));
	}
	if (!current()) return;
	reconcileRemoteRenderSelection();
	if (resetFrame) updateVideoLayout();
}

void MeetingRoomWindow::removeRemoteVideo(const QString &trackSid) {
	const auto binding = _remoteVideoBindings.find(trackSid);
	if (binding != _remoteVideoBindings.end() && _pinnedTrackKey &&
		binding->second.key == *_pinnedTrackKey) {
		_pinnedTrackKey.reset();
		_pinnedRenderKey.clear();
	}
	if (_remoteRenderSession) _remoteRenderSession->RemoveTrack(trackSid.toStdString());
	_activeRemoteRenderLeases.erase(trackSid);
	if (auto *tile = remoteVideoTile(trackSid)) {
		if (_videoCanvas) _videoCanvas->removeUser(tile->renderKey().toStdString());
		tile->setFrame({});
		tile->setVideoActive(false);
		tile->setVideoStreamPaused(false);
		if (_remoteScreenTiles.count(trackSid) && _pinnedRenderKey == tile->renderKey()) _pinnedRenderKey.clear();
	}
	_remoteScreenTiles.erase(trackSid);
	_remoteVideoBindings.erase(trackSid);
	syncVisibleRemoteTiles();
	reconcileRemoteRenderSelection();
	updateVideoLayout();
}

void MeetingRoomWindow::updateRemoteControlStatus(const QString& status) {
    if (_remoteControlStatus == status) return;
    _remoteControlStatus = status;
    if (!_screenShareBanner) return;
    _screenShareBanner->setText(_screenShareText + (status.isEmpty() ? QString() : "\n" + status));
    _remoteControlStopButton->setVisible(!status.isEmpty());
    QResizeEvent layout(size(), size());
    resizeEvent(&layout);
}

void MeetingRoomWindow::applyScreenShareSnapshot(livekit::ScreenShareSnapshot snapshot) {
	using State = livekit::ScreenShareState;
	_bottomBar->setScreenShareState(snapshot.state);
	if (!_screenShareBanner) {
		_screenShareBanner = new QLabel(this);
        _screenShareBanner->setObjectName(QStringLiteral("screenShareBanner"));
		_screenShareBanner->setTextFormat(Qt::PlainText);
		_screenShareBanner->setWordWrap(true);
		_screenShareBanner->setAlignment(Qt::AlignCenter);
        _screenShareBanner->setContentsMargins(136, 0, 110, 0);
        _screenQualityButton = new QPushButton(QCoreApplication::translate("MeetingUI", "Share quality"), _screenShareBanner);
        _screenQualityButton->setObjectName(QStringLiteral("screenShareQuality"));
        connect(_screenQualityButton, &QPushButton::clicked, this, [this] {
            if (!_coordinator || _coordinator->state() != OpenMeeting::MeetingState::InMeeting) return;
            const auto state = _coordinator->screenShareSnapshot();
            auto* dialog = new QDialog(this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setObjectName(QStringLiteral("screenShareQualityDialog"));
            dialog->setWindowTitle(QCoreApplication::translate("MeetingUI", "Screen share quality"));
            AppTheme::setTone(*dialog, AppTheme::Tone::Dark);
            AppTheme::makeDialogAdaptive(*dialog, QSize(420, 260));
            auto* layout = new QVBoxLayout(dialog);
            layout->addWidget(new QLabel(QCoreApplication::translate("MeetingUI", "Resolution (keeps aspect ratio; smaller sources are not enlarged)"), dialog));
            auto* resolution = createAccessibleComboBox(dialog);
            resolution->setObjectName(QStringLiteral("activeScreenShareResolution"));
            resolution->addItems({QCoreApplication::translate("MeetingUI", "Auto (up to 2K)"), QCoreApplication::translate("MeetingUI", "720p"), QCoreApplication::translate("MeetingUI", "1080p"), QCoreApplication::translate("MeetingUI", "1440p (2K)"), QCoreApplication::translate("MeetingUI", "Native (up to 4K)")});
            resolution->setCurrentIndex(static_cast<int>(state.requested_quality.resolution));
            layout->addWidget(resolution);
            auto* fps = createAccessibleComboBox(dialog);
            fps->setObjectName(QStringLiteral("activeScreenShareFps"));
            for (int rate : {15, 20, 30}) fps->addItem(QString::number(rate) + " FPS", rate);
            fps->setCurrentIndex(fps->findData(state.requested_quality.fps));
            layout->addWidget(fps);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, dialog);
            layout->addWidget(buttons);
            buttons->button(QDialogButtonBox::Apply)->setObjectName(QStringLiteral("screenShareQualityApply"));
            connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, dialog, [this, resolution, fps, dialog] {
                if (_coordinator) _coordinator->setScreenShareQuality({static_cast<livekit::ScreenShareResolution>(resolution->currentIndex()), fps->currentData().toInt()});
                dialog->accept();
            });
            connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
            dialog->open();
        });
		MeetingUI::AppTheme::setStyleVariant(*_screenShareBanner, "meeting-room-window-screensharebanner");
		_annotationButton = new QPushButton(QCoreApplication::translate("MeetingUI", "Annotate"), _screenShareBanner);
		_annotationButton->setObjectName(QStringLiteral("screenShareAnnotation"));
		connect(_annotationButton, &QPushButton::clicked,
			this, &MeetingRoomWindow::openAnnotationOverlay);
        _remoteControlStopButton = new QPushButton(tr("终止控制"), _screenShareBanner);
        _remoteControlStopButton->setObjectName(QStringLiteral("remoteControlStop"));
        connect(_remoteControlStopButton, &QPushButton::clicked, this, [this] {
            if (_remoteControlUi) _remoteControlUi->stop();
        });
	}
	const bool active = snapshot.state == State::Active;
	std::optional<livekit::ScreenBinding> nextBinding;
	if (active && snapshot.source_kind == livekit::DesktopSourceKind::Screen &&
		snapshot.annotation_binding) {
		nextBinding = snapshot.annotation_binding;
	}
	if (_annotationOverlay && (!nextBinding || !_annotationBinding ||
		*nextBinding != *_annotationBinding)) {
		closeAnnotationOverlay();
	}
	_annotationBinding = std::move(nextBinding);
	_screenShareText = active ? QCoreApplication::translate("MeetingUI", "Sharing: %1").arg(
		QString::fromStdString(snapshot.source_title).isEmpty() ? QCoreApplication::translate("MeetingUI", "Screen") :
		QString::fromStdString(snapshot.source_title)) :
		snapshot.state == State::Starting ? QCoreApplication::translate("MeetingUI", "Starting screen sharing...") :
		snapshot.state == State::StopFailed ? QCoreApplication::translate("MeetingUI", "Capture stopped, but unpublishing failed. Try stopping sharing again.") :
		QCoreApplication::translate("MeetingUI", "Stopping screen sharing...");
	_screenShareBanner->setVisible(active || snapshot.state == State::Starting ||
		snapshot.state == State::Stopping || snapshot.state == State::StopFailed);
    _screenQualityButton->setVisible(active);
    _screenQualityButton->setEnabled(active && _coordinator && _coordinator->state() == OpenMeeting::MeetingState::InMeeting &&
        snapshot.quality_status != livekit::ScreenShareQualityStatus::Degraded);
    QString qualityText;
    if (snapshot.applied_quality) qualityText = QCoreApplication::translate("MeetingUI", "%1 × %2, %3 FPS").arg(snapshot.applied_quality->width).arg(snapshot.applied_quality->height).arg(snapshot.applied_quality->quality.fps);
    switch (snapshot.quality_status) {
    case livekit::ScreenShareQualityStatus::Pending: qualityText += QCoreApplication::translate("MeetingUI", " — Applying…"); break;
    case livekit::ScreenShareQualityStatus::Rejected: qualityText += QCoreApplication::translate("MeetingUI", " — Change failed; previous quality restored"); break;
    case livekit::ScreenShareQualityStatus::MetadataPending: qualityText += QCoreApplication::translate("MeetingUI", " — Applied locally; synchronisation pending"); break;
    case livekit::ScreenShareQualityStatus::Degraded: qualityText += QCoreApplication::translate("MeetingUI", " — Quality state uncertain; stop sharing to recover"); break;
    default: break;
    }
    _screenQualityButton->setToolTip(qualityText);
    if (active) _screenShareText += "  " + qualityText;
    _screenShareBanner->setText(_screenShareText + (_remoteControlStatus.isEmpty() ? QString() : "\n" + _remoteControlStatus));
    _remoteControlStopButton->setVisible(active && !_remoteControlStatus.isEmpty());
	_annotationButton->setVisible(active);
	if (active && snapshot.source_kind == livekit::DesktopSourceKind::Window) {
		_annotationButton->setToolTip(QCoreApplication::translate(
			"MeetingUI", "Only full-screen sharing can be annotated."));
	} else if (active && !_annotationBinding) {
		_annotationButton->setToolTip(QCoreApplication::translate(
			"MeetingUI", "The shared screen could not be mapped reliably."));
	} else {
		_annotationButton->setToolTip(QCoreApplication::translate(
			"MeetingUI", "Annotate the shared screen"));
	}
	setAnnotationInteractionEnabled(
		_coordinator && _coordinator->state() == OpenMeeting::MeetingState::InMeeting);
	_localScreenPreview = active ? snapshot.preview : nullptr;
	if (active && !_localScreenTile) {
		_localScreenTile = std::make_unique<VideoTileWidget>(QCoreApplication::translate("MeetingUI", "My Screen Share"), true, _stageContainer, true);
		_localScreenTile->setIdentity(QStringLiteral("local-screen"));
		_localScreenTile->setVideoActive(true);
		bindTileInteractions(_localScreenTile.get());
	} else if (!active && _localScreenTile) {
		if (_videoCanvas) _videoCanvas->removeUser(_localScreenTile->renderKey().toStdString());
		if (_pinnedRenderKey == _localScreenTile->renderKey()) _pinnedRenderKey.clear();
		_localScreenTile.reset();
	}
	QResizeEvent layout(size(), size());
	resizeEvent(&layout);
}

void MeetingRoomWindow::openAnnotationOverlay() {
    if (_remoteControlUi && _remoteControlUi->active()) return;
	if (_annotationOverlay || !_annotationBinding || !_coordinator ||
		_coordinator->state() != OpenMeeting::MeetingState::InMeeting) {
		return;
	}
	try {
		auto overlay = std::make_unique<AnnotationOverlayWindow>(
			*_annotationBinding, !_annotationOffscreenForTesting);
		auto *raw = overlay.get();
		const QPointer<AnnotationOverlayWindow> guard(raw);
		connect(raw, &AnnotationOverlayWindow::closeRequested, this, [this, guard] {
			QMetaObject::invokeMethod(this, [this, guard] {
				if (_annotationOverlay.get() == guard.data()) closeAnnotationOverlay();
			}, Qt::QueuedConnection);
		});
		connect(raw, &AnnotationOverlayWindow::bindingInvalidated, this, [this, guard] {
			if (_annotationOverlay.get() != guard.data()) return;
			auto retired = std::move(_annotationOverlay);
			retired.release()->deleteLater();
			_annotationBinding.reset();
			if (_annotationButton) {
				_annotationButton->setEnabled(false);
				_annotationButton->setToolTip(QCoreApplication::translate(
					"MeetingUI", "The shared screen could not be mapped reliably."));
			}
			// The overlay's coordinates are invalid. Native ScreenShareSession
			// still validates source identity and owns capture termination.
		});
		_annotationOverlay = std::move(overlay);
	} catch (const std::exception &) {
		_annotationBinding.reset();
		if (_annotationButton) {
			_annotationButton->setEnabled(false);
			_annotationButton->setToolTip(QCoreApplication::translate(
				"MeetingUI", "The shared screen could not be mapped reliably."));
		}
		QMessageBox::warning(this,
			QCoreApplication::translate("MeetingUI", "Screen annotation"),
			QCoreApplication::translate("MeetingUI", "The shared screen could not be mapped reliably."));
	}
}

void MeetingRoomWindow::closeAnnotationOverlay() {
	if (!_annotationOverlay) return;
	_annotationOverlay->closeOverlay();
	_annotationOverlay.reset();
}

void MeetingRoomWindow::setAnnotationInteractionEnabled(bool enabled) {
    enabled = enabled && !(_remoteControlUi && _remoteControlUi->active());
    if (_screenQualityButton) _screenQualityButton->setEnabled(enabled && _coordinator && _coordinator->screenShareSnapshot().quality_status != livekit::ScreenShareQualityStatus::Degraded);
	if (_annotationOverlay) _annotationOverlay->setInteractionEnabled(enabled);
	if (_annotationButton) _annotationButton->setEnabled(enabled && _annotationBinding.has_value());
}

void MeetingRoomWindow::setupCoordinatorBindings() {
    setupRemoteControl();
	if (!_coordinator) return;
    connect(_topBar, &RoomTopBarWidget::telemetryDetailsRequested, this, [this] {
        if (_telemetryDialog) {
            _telemetryDialog->showNormal(); _telemetryDialog->raise(); _telemetryDialog->activateWindow();
            return;
        }
        const auto context = _coordinator->diagnosticContext();
        _telemetryDialog = OpenLiveTelemetryDialog(this,
            {std::string(context.anonymous_session_id.View()), context.session_generation},
            livekit::telemetry::InstalledTelemetryHistoryStore());
    });
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::sessionStopping,
		this, [this] {
            stopLiveKitSession(false);
        }, Qt::DirectConnection);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::telemetrySnapshotChanged,
		this, [this](const QVariantMap &snapshot) {
			_topBar->setTelemetrySnapshot(snapshot);
		});
	_bottomBar->shareScreenClicked() | rpl::on_next([this] { requestScreenShare(); }, lifetime());
	_bottomBar->setScreenShareAvailable(_coordinator->canStartScreenShare());
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::screenShareAvailabilityChanged,
		_bottomBar, &RoomBottomBarWidget::setScreenShareAvailable);
	applyScreenShareSnapshot(_coordinator->screenShareSnapshot());
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::screenShareChanged,
		this, [this](livekit::ScreenShareSnapshot snapshot) {
			applyScreenShareSnapshot(snapshot);
			if (snapshot.error == livekit::ScreenShareError::None) return;
			QString message;
			if (snapshot.error == livekit::ScreenShareError::Capture)
				message = QCoreApplication::translate("MeetingUI", "The selected screen or window is no longer available. Sharing has stopped. You can select another source.");
			else if (snapshot.error == livekit::ScreenShareError::Publish)
				message = QCoreApplication::translate("MeetingUI", "Unable to publish the screen share. Capture has stopped. Check your connection and publishing permissions, then try again.");
			else message = QCoreApplication::translate("MeetingUI", "Local capture has stopped, but remote unpublishing is not yet confirmed. Try stopping again or leave the meeting.");
			ShowMeetingWarning(this, "meetingScreenShareFailure", "meetingScreenShareFailureDismiss",
				QCoreApplication::translate("MeetingUI", "Screen Sharing"), message);
		});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::screenShareSourcesReady,
		this, [this](const std::vector<livekit::DesktopSource> &sources) {
			handleScreenShareSources(sources);
		});

	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::remoteVideoTrackAvailable,
	        this, [this](const QString &id, std::shared_ptr<livekit::Track> track) {
			if (!_coordinator) return;
			for (const auto &presentation : _coordinator->participantPresentations()) {
				if (presentation.participant.identity != id) continue;
				for (const auto &value : presentation.videoTracks) {
					if (value.track != track ||
						!_coordinator->isParticipantPresentationCurrent(presentation, &value)) continue;
					attachRemoteVideo(presentation, value);
					return;
				}
			}
		});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::videoDemandPlanAccepted,
		this, &MeetingRoomWindow::applyAcceptedVideoDemandPlan);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::remoteVideoTrackUnavailable,
	        this, [this](const QString &, const QString &trackSid) { removeRemoteVideo(trackSid); });
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::participantJoined,
	        this, [this](const QString &id, const QString &) {
		if (!_coordinator) return;
		for (const auto &presentation : _coordinator->participantPresentations()) {
			if (presentation.participant.identity != id) continue;
			applyParticipantPresentation(presentation);
			return;
		}
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::participantLeft,
	        this, &MeetingRoomWindow::onRemoteParticipantLeft);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::remoteTrackMuted,
	        this, [this](const QString &id, bool isVideo, bool muted) {
		onRemoteTrackMuted(id, isVideo, muted);
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::activeSpeakersChanged,
	        this, &MeetingRoomWindow::updateActiveSpeakers);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::meetingDetailUpdated,
	        this, &MeetingRoomWindow::onMeetingDetailUpdated);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::roomInfoUpdated,
	        this, [this](const OpenMeeting::MeetingRoomInfo &info) {
		if (!_coordinator || info.name.isEmpty() ||
			!_coordinator->meetingDetail().meetingName.isEmpty()) {
			return;
		}
		const QString meetingId = _coordinator->currentMeetingId();
		setWindowTitle(meetingId.isEmpty()
			? QCoreApplication::translate("MeetingUI", "Cohavora - %1").arg(info.name)
			: QCoreApplication::translate("MeetingUI", "Cohavora - %1 - Meeting ID: %2").arg(info.name, meetingId));
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::participantsUpdated,
	        this, [this](const std::vector<OpenMeeting::ParticipantInfo> &list) {
		_participantCount = static_cast<int>(list.size());
		_bottomBar->setParticipantCount(_participantCount);
		for (const auto &p : list) {
			if (p.isLocal) {
				if (_localTile) {
					_localTile->setConnectionQuality(p.connectionQuality);
				}
				continue;
			}
			_remoteParticipantNames[p.identity] =
				p.name.isEmpty() ? p.identity : p.name;
			auto it = _remoteTiles.find(p.identity);
			if (it != _remoteTiles.end() && it->second) {
				if (!p.name.isEmpty() && it->second->displayName() != p.name) {
					it->second->setDisplayName(p.name);
				}
				it->second->setAudioMuted(p.isAudioMuted);
				it->second->setConnectionQuality(p.connectionQuality);
			}
		}
		refreshRemoteVideoPresentations();
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::trackSubscriptionPermissionChanged,
	        this, [this](const QString &identity, const QString &participantSid,
	                     const QString &trackSid, bool allowed) {
		LogToConsole(LogCategory::Connection, "SUBSCRIPTION_PERMISSION",
			QCoreApplication::translate("MeetingUI", "Subscription permission updated: participant=%1 sid=%2 track=%3 allowed=%4")
				.arg(identity.isEmpty() ? QCoreApplication::translate("MeetingUI", "Unknown") : identity,
					 participantSid, trackSid, allowed ? QCoreApplication::translate("MeetingUI", "Yes") : QCoreApplication::translate("MeetingUI", "No")));
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::localAudioMuteChanged,
	        this, [this](bool muted) {
		_config.audioMuted = muted;
		_bottomBar->setAudioMuted(muted);
		_localTile->setAudioMuted(muted);
		if (_wasapiCap) {
			_wasapiCap->SetMute(muted);
		}
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::localVideoEnableChanged,
	        this, [this](bool enabled) {
		_config.videoEnabled = enabled;
		_bottomBar->setVideoEnabled(enabled);
		_localTile->setVideoActive(enabled && _usingRealCamera);
		updateVideoLayout();
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::kickedOff,
	        this, &MeetingRoomWindow::onKickedOff);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::meetingKickOff,
	        this, &MeetingRoomWindow::onMeetingKickOff);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::remoteMuteRequested,
	        this, &MeetingRoomWindow::onRemoteMuteRequested);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::hostRoleChanged,
	        this, &MeetingRoomWindow::onHostRoleChanged);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::meetingDetailUpdated,
	        this, &MeetingRoomWindow::onMeetingDetailUpdated);
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::stateChanged,
	        this, [this](OpenMeeting::MeetingState state, const QString &detail) {
		_bottomBar->setScreenShareAvailable(_coordinator->canStartScreenShare());
		if (state != OpenMeeting::MeetingState::InMeeting) {
			_defaultScreenSharePending = false;
		}
		if (state == OpenMeeting::MeetingState::Leaving
			|| state == OpenMeeting::MeetingState::Failed) {
			invalidateCameraCompletion();
			applyScreenShareSnapshot({});
		}
		QPointer<MeetingRoomWindow> guard(this);
		updateRecoveryStateUi(state, detail);
		if (!guard) {
			return;
		}
		if (state == OpenMeeting::MeetingState::InMeeting) {
			_room = _coordinator->room();
			const auto preferences = OpenMeeting::SessionManager::instance().mediaPreferences();
			if (!selectSpeakerDevice(preferences.speakerDeviceId)
				&& !preferences.speakerDeviceId.isEmpty()) {
				selectSpeakerDevice(QString());
			}
			setSpeakerOutputMuted(!preferences.enableSpeaker);
			restoreParticipantPresentations();
		}
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::errorOccurred,
	        this, [this](const QString &title, const QString &message) {
		if (_closingForSessionInvalidation ||
			OpenMeeting::SessionManager::instance().isSessionInvalidating() ||
			!_coordinator || _coordinator->state() != OpenMeeting::MeetingState::Failed) {
			return;
		}
		invalidateCameraCompletion();
		QPointer<MeetingRoomWindow> guard(this);
		LogToConsole(LogCategory::Error, "SESSION_STARTUP", QString("%1: %2").arg(title, message));
		if (!guard) {
			return;
		}
		showDepartureNotice(title, message, QMessageBox::Critical);
	});
	connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::meetingLeft,
	        this, [this]() {
		invalidateCameraCompletion();
		if (!_pendingDepartureTitle.isEmpty()) {
			// Capture callbacks were revoked by sessionStopping; the non-modal
			// notice remains responsive while native cleanup finishes separately.
			stopLiveKitSession();
			const auto title = _pendingDepartureTitle;
			const auto message = _pendingDepartureMessage;
			_pendingDepartureTitle.clear();
			_pendingDepartureMessage.clear();
			showDepartureNotice(title, message);
			return;
		}
		if (_departureNotice) {
			// Revoke presentation immediately while capture retirement completes.
			stopLiveKitSession();
			return;
		}
		close();
	});

	if (!_coordinator->currentMeetingId().isEmpty()) {
		if (_topBar) _topBar->setMeetingId(_coordinator->currentMeetingId());
		setWindowTitle(QCoreApplication::translate("MeetingUI", "Cohavora - Meeting ID: %1").arg(_coordinator->currentMeetingId()));
	}

	updateRecoveryStateUi(_coordinator->state());
}

void MeetingRoomWindow::showDepartureNotice(const QString &title, const QString &message,
		QMessageBox::Icon icon) {
	if (_departureNotice) return;
	// A screen-sized TOPMOST annotation surface can otherwise remain above the
	// meeting-owned dialog during the short kick/leave transition.
	closeAnnotationOverlay();
	_annotationBinding.reset();
	// Do not retain the retired meeting HWND as the native owner.  In particular,
	// QDialog::open() creates a window-modal owner chain while Room teardown is
	// changing that owner's native state.  A standalone non-modal notice keeps
	// its input HWND enabled and still avoids a nested event loop.
	auto *notice = new QMessageBox(icon, title, message, QMessageBox::Ok, nullptr);
	notice->setObjectName("meetingDepartureNotice");
	notice->setAttribute(Qt::WA_DeleteOnClose);
	notice->setAttribute(Qt::WA_QuitOnClose, false);
	notice->setWindowModality(Qt::NonModal);
	notice->setWindowFlag(Qt::WindowStaysOnTopHint, true);
	notice->setMinimumSize(460, 180);
	AppTheme::setTone(*notice, AppTheme::Tone::Light);
	_departureNotice = notice;
	connect(notice, &QDialog::finished, this, [this]() {
		_departureNotice.clear();
		_pendingDepartureTitle.clear();
		_pendingDepartureMessage.clear();
		close();
	});
	notice->show();
	notice->raise();
	notice->activateWindow();
}

void MeetingRoomWindow::onKickedOff(const QString &reason, int reasonCode) {
	if (_closingForSessionInvalidation ||
		OpenMeeting::SessionManager::instance().isSessionInvalidating()) {
		return;
	}
	invalidateCameraCompletion();
	if (_pendingDepartureTitle.isEmpty()) {
		_pendingDepartureTitle = QCoreApplication::translate("MeetingUI", "Remove from Meeting");
		_pendingDepartureMessage = QCoreApplication::translate(
			"MeetingUI", "You were removed from the meeting by the host.\nReason: %1 (code: %2)")
			.arg(reason.isEmpty() ? QCoreApplication::translate("MeetingUI", "Not Specified") : reason)
			.arg(reasonCode);
	}
}

void MeetingRoomWindow::onMeetingKickOff(livekit::RoomDisconnectReason reason) {
	if (reason != livekit::RoomDisconnectReason::DuplicateIdentity) {
		return;
	}
	if (_closingForSessionInvalidation ||
		OpenMeeting::SessionManager::instance().isSessionInvalidating()) {
		LogToConsole(LogCategory::Connection, "DUPLICATE_IDENTITY_SUPPRESSED",
		             "[UI] Suppress room-level dialog while account session is invalidating");
		return;
	}

	invalidateCameraCompletion();
	QPointer<MeetingRoomWindow> guard(this);
	LogToConsole(LogCategory::Connection, "DUPLICATE_IDENTITY",
	             "[UI] Show duplicate login dialog");
	if (!guard) return;
	showDepartureNotice(
	                     QCoreApplication::translate("MeetingUI", "Meeting Left"),
	                     QCoreApplication::translate("MeetingUI", "Your account joined this meeting on another device. This client has been disconnected."));
}

void MeetingRoomWindow::onSessionInvalidated(OpenMeeting::SessionInvalidationReason reason) {
	invalidateCameraCompletion();
	if (_closingForSessionInvalidation) {
		return;
	}
	_closingForSessionInvalidation = true;
	QPointer<MeetingRoomWindow> guard(this);
	LogToConsole(LogCategory::Connection, "SESSION_INVALIDATED",
	             QString("[UI] Close meeting window for invalidated account session, reason=%1")
	                 .arg(static_cast<int>(reason)));

	// 不在会议窗口重复弹窗；全局提示和重新登录由 MeetingMainWindow 统一负责。
	if (guard) {
		guard->close();
	}
}

void MeetingRoomWindow::onRemoteMuteRequested(bool isVideo, bool mute, const QString &operatorId) {
	const auto operatorName = resolveParticipantDisplayName(_coordinator, operatorId, QString());
	if (isVideo) {
		if (mute) {
			_bottomBar->setVideoEnabled(false);
			_config.videoEnabled = false;
			_localTile->setVideoActive(false);
			if (_coordinator) _coordinator->setLocalVideoEnabled(false);
			updateVideoLayout();
			LogToConsole(LogCategory::Media, "VIDEO", QCoreApplication::translate("MeetingUI", "Host [%1] turned off your camera").arg(operatorName));
		} else {
			if (QMessageBox::question(this, QCoreApplication::translate("MeetingUI", "Request to Enable Camera"),
				QCoreApplication::translate("MeetingUI", "Host [%1] would like you to turn on your camera. Allow?").arg(operatorName),
				QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
				_bottomBar->setVideoEnabled(true);
				_config.videoEnabled = true;
				_localTile->setVideoActive(_usingRealCamera);
				requestLocalCamera(true);
				updateVideoLayout();
			}
		}
	} else {
		if (mute) {
			_bottomBar->setAudioMuted(true);
			_config.audioMuted = true;
			_localTile->setAudioMuted(true);
			if (_wasapiCap) _wasapiCap->SetMute(true);
			if (_coordinator) _coordinator->setLocalAudioMuted(true);
			LogToConsole(LogCategory::Media, "AUDIO", QCoreApplication::translate("MeetingUI", "Host [%1] muted you").arg(operatorName));
		} else {
			if (QMessageBox::question(this, QCoreApplication::translate("MeetingUI", "Request to Unmute"),
				QCoreApplication::translate("MeetingUI", "Host [%1] would like you to unmute your microphone. Allow?").arg(operatorName),
				QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
				_bottomBar->setAudioMuted(false);
				_config.audioMuted = false;
				_localTile->setAudioMuted(false);
				if (_wasapiCap) _wasapiCap->SetMute(false);
				requestLocalMicrophone(false);
			}
		}
	}
}

void MeetingRoomWindow::onMeetingDetailUpdated(const OpenMeeting::MeetingDetail &detail) {
	if (!detail.meetingName.isEmpty()) {
		setWindowTitle(QCoreApplication::translate("MeetingUI", "Cohavora - %1 - Meeting ID: %2").arg(detail.meetingName).arg(detail.meetingId));
	}
	if (_topBar && !detail.meetingId.isEmpty()) {
		_topBar->setMeetingId(detail.meetingId);
	}
	if (_participantsSidebar) {
		_participantsSidebar->updateParticipants(_coordinator ? _coordinator->participants() : std::vector<OpenMeeting::ParticipantInfo>{});
	}
}

void MeetingRoomWindow::onHostRoleChanged(const QString &newHostId, const QString &operatorName) {
	LogToConsole(LogCategory::Participant, "HOST", QCoreApplication::translate("MeetingUI", "Host transferred to: %1 (by: %2)").arg(newHostId).arg(operatorName));
	QMessageBox::information(this, QCoreApplication::translate("MeetingUI", "Host Changed"),
	                         QCoreApplication::translate("MeetingUI", "Participant [%1] is now the host").arg(newHostId));
}

void MeetingRoomWindow::handleEndMeetingClicked() {
	if (_preparingMedia) { close(); return; }
	if (_coordinator && _coordinator->isHost()) {
		QMessageBox box(this);
		box.setObjectName(QStringLiteral("meetingLeaveConfirmation"));
		box.setWindowTitle(QCoreApplication::translate("MeetingUI", "End Meeting"));
		box.setText(QCoreApplication::translate("MeetingUI", "You are the host. How would you like to leave?"));
		auto *leaveBtn = box.addButton(QCoreApplication::translate("MeetingUI", "Leave Only"), QMessageBox::ActionRole);
		auto *endBtn = box.addButton(QCoreApplication::translate("MeetingUI", "End for Everyone"), QMessageBox::DestructiveRole);
		auto *cancelBtn = box.addButton(QCoreApplication::translate("MeetingUI", "Cancel"), QMessageBox::RejectRole);
		leaveBtn->setObjectName(QStringLiteral("meetingLeaveConfirm"));
		endBtn->setObjectName(QStringLiteral("meetingEndForEveryone"));
		cancelBtn->setObjectName(QStringLiteral("meetingLeaveCancel"));
		box.exec();
		if (box.clickedButton() == leaveBtn) {
			QPointer<MeetingRoomWindow> guard(this);
			stopLiveKitSession(false);
			if (!guard) return;
			_coordinator->leaveMeetingAsync(false);
			if (guard) guard->close();
		} else if (box.clickedButton() == endBtn) {
			QPointer<MeetingRoomWindow> guard(this);
			stopLiveKitSession(false);
			if (!guard) return;
			_coordinator->leaveMeetingAsync(true);
			if (guard) guard->close();
		}
	} else {
		QMessageBox box(QMessageBox::Question,
			QCoreApplication::translate("MeetingUI", "Leave Meeting"),
			QCoreApplication::translate("MeetingUI", "Are you sure you want to leave this meeting?"),
			QMessageBox::Yes | QMessageBox::No, this);
		box.setObjectName(QStringLiteral("meetingLeaveConfirmation"));
		box.button(QMessageBox::Yes)->setObjectName(QStringLiteral("meetingLeaveConfirm"));
		box.button(QMessageBox::No)->setObjectName(QStringLiteral("meetingLeaveCancel"));
		if (box.exec() == QMessageBox::Yes) {
			QPointer<MeetingRoomWindow> guard(this);
			stopLiveKitSession(false);
			if (!guard) return;
			if (_coordinator) {
				_coordinator->leaveMeetingAsync(false);
			}
			if (guard) guard->close();
		}
	}
}

void MeetingRoomWindow::setupInvitationBinding() {
	if (!_bottomBar) return;
	_bottomBar->inviteClicked() | rpl::on_next([this] {
		handleInviteClicked();
	}, lifetime());
}

void MeetingRoomWindow::handleInviteClicked() {
	if (_config.invitationMode != InvitationMode::BusinessMeetingId) {
		QPointer<MeetingRoomWindow> guard(this);
		LogToConsole(LogCategory::General, "INVITE", QCoreApplication::translate("MeetingUI", "This connection does not support meeting ID invitations"));
		if (guard) {
			guard->showInvitationNotice(false, QCoreApplication::translate("MeetingUI", "Unable to Copy Invitation"),
				QCoreApplication::translate("MeetingUI", "This connection does not support meeting ID invitations. Ask the organizer to provide another way for participants to join."));
		}
		return;
	}

	const auto ready = _coordinator
		&& _coordinator->state() == OpenMeeting::MeetingState::InMeeting;
	const auto meetingId = ready ? _coordinator->currentMeetingId() : QString();
	bool validMeetingId = !meetingId.isEmpty();
	for (const QChar character : meetingId) {
		if (character.isSpace() || !character.isPrint()) {
			validMeetingId = false;
			break;
		}
	}
	if (!validMeetingId) {
		QPointer<MeetingRoomWindow> guard(this);
		LogToConsole(LogCategory::General, "INVITE", QCoreApplication::translate("MeetingUI", "The meeting is not ready. No invitation was copied."));
		if (guard) {
			guard->showInvitationNotice(false, QCoreApplication::translate("MeetingUI", "Invitation Unavailable"),
				QCoreApplication::translate("MeetingUI", "The meeting is not ready. The invitation cannot be copied yet."));
		}
		return;
	}

	const auto inviteText = QCoreApplication::translate("MeetingUI", "[Cohavora Meeting Invitation]\nMeeting ID: %1\nSign in with your own account in a client configured for the same meeting service, then join using this meeting ID.")
		.arg(meetingId);
	QPointer<MeetingRoomWindow> guard(this);
	QApplication::clipboard()->setText(inviteText);
	if (!guard) return;
	LogToConsole(LogCategory::General, "INVITE", QCoreApplication::translate("MeetingUI", "Meeting invitation copied to the clipboard"));
	if (guard) {
		guard->showInvitationNotice(true, QCoreApplication::translate("MeetingUI", "Invitation Copied"),
			QCoreApplication::translate("MeetingUI", "The meeting invitation has been copied to the clipboard. You can send it to other participants."));
	}
}

void MeetingRoomWindow::showInvitationNotice(
		bool success,
		const QString &title,
		const QString &message) {
	if (_invitationNoticeEffect) {
		auto effect = _invitationNoticeEffect;
		effect(success, title, message);
		return;
	}
	if (success) {
		QMessageBox::information(this, title, message);
	} else {
		QMessageBox::warning(this, title, message);
	}
}

void MeetingRoomWindow::bindLocalMediaSources() {
	if (!_captureUiCallbacks) {
		_captureUiCallbacks = OpenMeeting::QtCallbackGate<MeetingRoomWindow>::Create(this);
	}
	if (_coordinator) {
		_localAudioSource = _coordinator->localAudioSource();
		_localVideoSource = _coordinator->localVideoSource();
	}
	if (!_localAudioSource) {
		_localAudioSource = std::make_shared<livekit::AudioSource>(48000, 2);
	}
	if (!_localVideoSource) {
		_localVideoSource = std::make_shared<livekit::VideoSource>(1280, 720);
	}
	if (_remoteRenderSession && _config.videoEnabled) {
		_remoteRenderSession->AttachLocalSource(_localVideoSource);
	}
}

void MeetingRoomWindow::attachCoordinatorSession() {
	if (!_coordinator) return;
    if (!_encryptionPanel) {
        _encryptionPanel = new MeetingEncryptionPanel(this);
        _encryptionPanel->setRecoveryHandler([weak = std::weak_ptr<OpenMeeting::MeetingCoordinator>(_coordinator)](
            std::shared_ptr<livekit::MeetingSecretHandle> secret) {
            if (const auto owner = weak.lock()) owner->recoverEncryptionKey(std::move(secret));
            else if (secret) secret->Revoke();
        });
        const auto refresh = [this] {
            _encryptionPanel->setSession(_coordinator->requiresEncryption(),
                _coordinator->state() == OpenMeeting::MeetingState::InMeeting,
                _coordinator->canRecoverEncryptionKey());
        };
        connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::stateChanged, this,
            [refresh](OpenMeeting::MeetingState, const QString&) { refresh(); });
        // Native generation may arrive after InMeeting; it is published before
        // participantsUpdated, so refresh recovery availability at that boundary.
        connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::participantsUpdated, this,
            [refresh](const auto&) { refresh(); });
        connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::encryptionKeyRecoveryFinished, this,
            [this](bool installed) {
                _encryptionPanel->setInstallResult(installed, _coordinator->canRecoverEncryptionKey());
            });
        connect(_coordinator.get(), &OpenMeeting::MeetingCoordinator::encryptionMediaStatusChanged, this,
            [this](const livekit::MediaEncryptionStatus& status) { _encryptionPanel->setMediaStatus(status); });
        // Bounded display sampling, never a recovery retry or an acceptance test.
        auto* encryptionStatusTimer = new QTimer(_encryptionPanel);
        encryptionStatusTimer->setInterval(1000);
        connect(encryptionStatusTimer, &QTimer::timeout, this,
            [this] { _coordinator->refreshEncryptionMediaStatus(); });
        encryptionStatusTimer->start();
        refresh();
        _encryptionPanel->show();
        QResizeEvent layoutEvent(size(), size());
        resizeEvent(&layoutEvent);
    }


	// The entry owner starts the coordinator after constructing this window.
	// Connecting here as well would restart that session, replace its sources,
	// and leave the capture devices feeding only this window's old sources.
	// Arm cleanup before asynchronous preparation or admission begins.
	_sessionRunning = true;
	_room = _coordinator->room();
	if (_coordinator->state() == OpenMeeting::MeetingState::InMeeting) {
		restoreParticipantPresentations();
	}
}

void MeetingRoomWindow::stopLiveKitSession(bool requestLeave) {
	Q_ASSERT(thread() == QThread::currentThread());
	// The Coordinator's cleanup owner retains native resources through drain.
	// A hidden/departure-notice window must not retain the retired Room and keys.
	_room.reset();
	CloseLiveTelemetryDialog(_telemetryDialog);
	const bool wasRunning = _sessionRunning.exchange(false);
	cancelMediaPreparation();
	if (_captureUiCallbacks) _captureUiCallbacks->Revoke();
	invalidateCameraCompletion();
	cancelDeviceSwitchTelemetry();
	closeAnnotationOverlay();
	_annotationBinding.reset();
	_localScreenPreview.reset();
	if (_remoteRenderSession) _remoteRenderSession->Deactivate();
	if (_meetingTimer) _meetingTimer->stop();
	if (_remoteRenderTimer) _remoteRenderTimer->stop();
	if (_localGenTimer) _localGenTimer->stop();
	if (_recoveryBannerFadeTimer) _recoveryBannerFadeTimer->stop();
	_usingGpuBackend.store(false, std::memory_order_release);
	if (_videoCanvas) {
		_videoCanvas->shutdownRenderer();
		_videoCanvas->hide();
	}

	if (_wasapiCap) {
		_wasapiCap->SetMute(true);
		_wasapiCap->SetCaptureStateCallback({});
		_wasapiCap->SetDeviceFrameCallback({});
		auto &manager = livekit::WebRTCManager::Instance();
		if (manager.apm_processor() == _wasapiCap->apm_processor()) {
			manager.SetApmProcessor(nullptr);
		}
	}
	retireLocalCapture();

	if (wasRunning && requestLeave && _coordinator &&
		_coordinator->state() != OpenMeeting::MeetingState::Failed) {
		_coordinator->leaveMeetingAsync(false);
	}
	if (wasRunning) {
		LogToConsole(LogCategory::Connection, "DISCONNECT", QCoreApplication::translate("MeetingUI", "Stopping"));
	}
}

#if defined(Q_OS_WIN)
int MeetingRoomWindow::nativeResizeHitTest(LPARAM position) const {
	if (!_handle || isMaximized() || isFullScreen()) return HTCLIENT;
	POINT point{ GET_X_LPARAM(position), GET_Y_LPARAM(position) };
	RECT client{};
	if (!ScreenToClient(_handle, &point) || !GetClientRect(_handle, &client) ||
		!PtInRect(&client, point)) return HTCLIENT;
	const int sideBorder = std::max(1, qRound(8 * devicePixelRatioF()));
	// The native frame already provides an outer resize edge. Only a narrow
	// inner vertical band is needed beside the top/bottom controls.
	const int verticalBorder = std::max(1,
		qRound(4 * devicePixelRatioF()) - kNativeVerticalFramePixels);
	const bool left = point.x < sideBorder;
	const bool right = point.x >= client.right - sideBorder;
	const bool top = point.y < verticalBorder;
	const bool bottom = point.y >= client.bottom - verticalBorder;
	if (top && left) return HTTOPLEFT;
	if (top && right) return HTTOPRIGHT;
	if (bottom && left) return HTBOTTOMLEFT;
	if (bottom && right) return HTBOTTOMRIGHT;
	if (left) return HTLEFT;
	if (right) return HTRIGHT;
	if (top) return HTTOP;
	if (bottom) return HTBOTTOM;
	return HTCLIENT;
}
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool MeetingRoomWindow::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
#else
bool MeetingRoomWindow::nativeEventFilter(const QByteArray &eventType, void *message, long *result)
#endif
{
	Q_UNUSED(eventType);
#if defined(Q_OS_WIN)
	const auto msg = static_cast<MSG*>(message);
	if (msg && msg->message == WM_NCHITTEST && _handle && msg->hwnd != _handle &&
		IsChild(_handle, msg->hwnd) && nativeResizeHitTest(msg->lParam) != HTCLIENT) {
		// The GPU canvas and its paging overlay have native child HWNDs.
		// Let Windows continue hit-testing to the meeting's top-level HWND at
		// resize edges. Returning HTLEFT/etc. here would resize the child itself.
		*result = HTTRANSPARENT;
		return true;
	}
#endif
	return false;
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool MeetingRoomWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
#else
bool MeetingRoomWindow::nativeEvent(const QByteArray &eventType, void *message, long *result)
#endif
{
#if defined(Q_OS_WIN)
	auto msg = reinterpret_cast<MSG*>(message);
	if (!msg) {
		return Ui::RpWidget::nativeEvent(eventType, message, result);
	}
	// Once the DX11 Canvas creates a native child HWND, Qt may forward a native
	// message whose hwnd is that child. Hit-testing and resizing are defined in
	// MeetingRoomWindow coordinates, so never use the child as the conversion
	// origin here.
	HWND handle = _handle ? _handle : msg->hwnd;

	switch (msg->message) {
	case WM_ERASEBKGND: {
		if (_handle && msg->hwnd == _handle) {
			// Qt's backing store paints the opaque client area in one pass.
			*result = 1;
			return true;
		}
	} break;

	case WM_NCACTIVATE: {
		if (_handle && msg->hwnd == _handle) {
			// Preserve activation state without drawing a native caption over Qt.
			*result = DefWindowProcW(_handle, msg->message, msg->wParam, -1);
			return true;
		}
	} break;

	case WM_NCCALCSIZE: {
		if (msg->hwnd == handle && msg->wParam == TRUE) {
			auto *parameters = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
			const RECT proposed = parameters->rgrc[0];
			DefWindowProcW(handle, msg->message, msg->wParam, msg->lParam);
			// Keep a native frame on all four sides and replace the caption with
			// our Qt top bar. A thin top/bottom edge avoids a visible system band
			// without taking over the entire non-client area beside GPU HWNDs.
			const int resizeBorder = proposed.bottom - parameters->rgrc[0].bottom;
			const auto style = GetWindowLongPtrW(handle, GWL_STYLE);
			if ((style & WS_THICKFRAME) && !(style & WS_MAXIMIZE) &&
				!isFullScreen() && resizeBorder > 0) {
				// NCCALCSIZE can precede Qt's screen/DPI notification. A single
				// physical pixel keeps this frame stable across that transition.
				const int thinBorder = std::min(resizeBorder, kNativeVerticalFramePixels);
				parameters->rgrc[0].top = proposed.top + thinBorder;
				parameters->rgrc[0].bottom = proposed.bottom - thinBorder;
			} else {
				// Maximized windows still need the system's off-work-area inset.
				parameters->rgrc[0].top = proposed.top + resizeBorder;
			}
			*result = 0;
			return true;
		}
	} break;

	case WM_NCHITTEST: {
		if (!handle) break;
		POINT nativePoint{ GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam) };
		RECT client{};
		if (ScreenToClient(handle, &nativePoint) && GetClientRect(handle, &client) &&
			!PtInRect(&client, nativePoint)) {
			*result = DefWindowProcW(handle, msg->message, msg->wParam, msg->lParam);
			return true;
		}
		const int resizeHit = nativeResizeHitTest(msg->lParam);
		if (resizeHit != HTCLIENT) {
			*result = resizeHit;
			return true;
		}

		POINT p{ GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam) };
		ScreenToClient(handle, &p);

		const qreal ratio = devicePixelRatioF();
		const int x = static_cast<int>(p.x / ratio);
		const int y = static_cast<int>(p.y / ratio);

		// Alien title-bar widgets share this HWND. Only its actual empty area
		// may act as a caption; its controls must receive client mouse events.
		if (_topBar && _topBar->isWindowDragArea(
			_topBar->mapFrom(this, QPoint(x, y)))) {
			*result = HTCAPTION;
			return true;
		}

		*result = HTCLIENT;
		return true;
	} break;
	}
#endif
	return Ui::RpWidget::nativeEvent(eventType, message, result);
}

} // namespace MeetingUI
