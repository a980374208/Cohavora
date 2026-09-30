#include "src/ui/app_theme.h"
#include "src/ui/app_branding.h"
#include "src/ui/app_translation.h"
#include "src/ui/meeting_list_model.h"
#include "tests/support/test_check.h"

#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtCore/QTranslator>
#include <QtGui/QPalette>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtPlugin>

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin)
Q_IMPORT_PLUGIN(QSvgPlugin)
Q_IMPORT_PLUGIN(QSvgIconPlugin)
Q_IMPORT_PLUGIN(QJpegPlugin)
Q_IMPORT_PLUGIN(QGifPlugin)
Q_IMPORT_PLUGIN(QICOPlugin)

namespace {
bool styleWarning = false;
void messageHandler(QtMsgType, const QMessageLogContext &, const QString &message) {
	if (message.contains(QStringLiteral("style sheet"), Qt::CaseInsensitive)
		|| message.contains(QStringLiteral("Unknown property"))) {
		styleWarning = true;
		fprintf(stderr, "%s\n", message.toUtf8().constData());
	}
}
void polish(QWidget &widget) {
	widget.ensurePolished();
	QCoreApplication::processEvents();
}
} // namespace

int main(int argc, char **argv) {
	QApplication app(argc, argv);
	qInstallMessageHandler(messageHandler);
	using namespace MeetingUI;
	// Startup installs Chinese before widgets are created, even on an English OS.
	QLocale::setDefault(QLocale(QStringLiteral("en_US")));
	TEST_CHECK(AppTranslation::startupLocale({ "app" }).name() == "zh_CN");
	TEST_CHECK(AppTranslation::startupLocale({ "app", "--language=zh_CN" }).name() == "zh_CN");
	TEST_CHECK(AppTranslation::startupLocale({ "app", "--language", "en_US" }).name() == "en_US");
	TEST_CHECK(AppTranslation::startupLocale({ "app", "--language=en_US" }).name() == "en_US");
	AppTranslation::install(app, AppTranslation::startupLocale(app.arguments()));
	if (app.arguments().contains(QStringLiteral("--language=en_US"))) {
		TEST_CHECK(QLocale().name() == QStringLiteral("en_US"));
		TEST_CHECK(QCoreApplication::translate("MeetingUI", "Settings") == QStringLiteral("Settings"));
		TEST_CHECK(app.findChildren<QTranslator *>().isEmpty());
		qInstallMessageHandler(nullptr);
		return 0;
	}
	TEST_CHECK(QLocale().name() == QStringLiteral("zh_CN"));
	TEST_CHECK(QCoreApplication::translate("MeetingUI", "Settings") == QString::fromUtf8("设置"));
	TEST_CHECK(QCoreApplication::translate("MeetingUI", "Meeting ID %1").arg(123) == QString::fromUtf8("会议号 123"));
	TEST_CHECK(QCoreApplication::translate("MeetingUI", "Join Meeting") == QString::fromUtf8("加入会议"));
	TEST_CHECK(QCoreApplication::translate("MeetingUI", "Meeting telemetry") ==
		QString::fromUtf8("会议遥测"));
	app.setApplicationDisplayName(AppBranding::displayName());
	TEST_CHECK(app.applicationDisplayName() ==
		QString::fromUtf8("Cohavora · 开源音视频会议客户端"));
	TEST_CHECK(QCoreApplication::translate("MeetingUI", "Visible render stall") ==
		QString::fromUtf8("用户可见画面冻结"));
	TEST_CHECK(QCoreApplication::translate("MeetingUI", "Capability boundaries") ==
		QString::fromUtf8("能力边界"));
	TEST_CHECK(QCoreApplication::translate("TelemetryDisplay", "VALID") ==
		QString::fromUtf8("有效"));
	TEST_CHECK(QCoreApplication::translate(
		"TelemetryDisplay", "IMPLEMENTED_DETERMINISTIC") ==
		QString::fromUtf8("已实现且可确定测量"));
	TEST_CHECK(QCoreApplication::translate(
		"TelemetryDisplay", "signaling_restored_waiting_for_video") ==
		QString::fromUtf8("信令已恢复，等待视频稳定"));
	TEST_CHECK(QCoreApplication::translate(
		"MeetingUI", "Telemetry: %1, age %2 ms")
			.arg(QString::fromUtf8("有效"), QStringLiteral("32")) ==
		QString::fromUtf8("遥测：有效，数据时效 32 ms"));
	TEST_CHECK(QCoreApplication::translate("QPlatformTheme", "Cancel") == QString::fromUtf8("取消"));
	const auto translatorCount = app.findChildren<QTranslator *>().size();
	TEST_CHECK(translatorCount > 0);
	AppTranslation::install(app, QLocale(QStringLiteral("zh_CN")));
	TEST_CHECK(app.findChildren<QTranslator *>().size() == translatorCount);
	QProcess english;
	english.start(QCoreApplication::applicationFilePath(), { QStringLiteral("--language=en_US") });
	TEST_CHECK(english.waitForFinished(10000));
	TEST_CHECK(english.exitStatus() == QProcess::NormalExit && english.exitCode() == 0);

	AppTheme::install(app);
	const auto originalSheet = app.styleSheet();
	TEST_CHECK(!originalSheet.isEmpty());
	AppTheme::install(app);
	TEST_CHECK(app.styleSheet() == originalSheet);
	QWidget form;
	form.setObjectName(QStringLiteral("settingsDialog"));
	AppTheme::setStyleVariant(form, "settings-dialog-this");
	QLabel status(&form);
	status.setObjectName(QStringLiteral("hintLabel"));
	AppTheme::setStyleVariant(status, "settings-dialog-audiostatus");
	polish(status);
	TEST_CHECK(status.palette().color(QPalette::WindowText) == QColor("#f53f3f"));
	AppTheme::setStyleVariant(status, "settings-dialog-audiostatus-2-active");
	TEST_CHECK(status.palette().color(QPalette::WindowText) == QColor("#20a162"));
	TEST_CHECK(status.styleSheet().isEmpty() && form.styleSheet().isEmpty());

	QLabel preview(&form);
	preview.setObjectName(QStringLiteral("previewMessage"));
	AppTheme::setStyleVariant(preview, "settings-dialog-previewmessage-active");
	polish(preview);
	TEST_CHECK(preview.palette().color(QPalette::WindowText) == QColor("#ff7875"));
	AppTheme::setStyleVariant(preview, "settings-dialog-previewmessage-normal");
	TEST_CHECK(preview.palette().color(QPalette::WindowText) != QColor("#ff7875"));

	QMenu menu;
	AppTheme::styleMenu(menu, AppTheme::Tone::Dark);
	polish(menu);
	TEST_CHECK(menu.palette().color(QPalette::Window) == QColor("#1a1d24"));
	AppTheme::styleMenu(menu, AppTheme::Tone::Light);
	TEST_CHECK(menu.palette().color(QPalette::Window) == QColor("#ffffff"));

	QWidget darkPanel;
	AppTheme::setTone(darkPanel, AppTheme::Tone::Dark);
	QMessageBox box(QMessageBox::Information, QStringLiteral("Test"),
		QStringLiteral("Prompt"), QMessageBox::Ok | QMessageBox::Cancel, &darkPanel);
	polish(box);
	TEST_CHECK(box.property("meetingUiTone").toByteArray() == "light");
	TEST_CHECK(box.styleSheet().isEmpty());
	TEST_CHECK(box.button(QMessageBox::Ok)->property("meetingUiRole").toByteArray() == "primary");
	TEST_CHECK(box.button(QMessageBox::Cancel)->property("meetingUiRole").toByteArray() == "secondary");

	// Long translations and large fonts remain reachable on a small window.
	QDialog adaptive;
	adaptive.setAttribute(Qt::WA_DontShowOnScreen);
	adaptive.setFixedSize(460, 600);
	auto *adaptiveLayout = new QVBoxLayout(&adaptive);
	auto *longLabel = new QLabel(QStringLiteral("A longer translated message with several words. ").repeated(60), &adaptive);
	auto largeFont = longLabel->font();
	largeFont.setPointSize(18);
	longLabel->setFont(largeFont);
	adaptiveLayout->addWidget(longLabel);
	auto *adaptiveForm = new QFormLayout;
	adaptiveForm->addRow(QStringLiteral("A long translated field label"), new QLineEdit(&adaptive));
	adaptiveLayout->addLayout(adaptiveForm);
	auto *lastButton = new QPushButton(QStringLiteral("Continue"), &adaptive);
	adaptiveLayout->addWidget(lastButton);
	AppTheme::makeDialogAdaptive(adaptive, QSize(460, 600));
	adaptive.show();
	polish(adaptive);
	adaptive.resize(320, 240);
	polish(adaptive);
	auto *scroll = adaptive.findChild<QScrollArea *>(QStringLiteral("adaptiveDialogScroll"));
	TEST_CHECK(scroll && longLabel->wordWrap());
	TEST_CHECK(adaptiveForm->rowWrapPolicy() == QFormLayout::WrapLongRows);
	TEST_CHECK(adaptive.width() == 320 && adaptive.height() == 240);
	TEST_CHECK(longLabel->height() >= longLabel->heightForWidth(longLabel->width()));
	TEST_CHECK(scroll->verticalScrollBar()->maximum() > 0);
	scroll->ensureWidgetVisible(lastButton);
	polish(adaptive);
	TEST_CHECK(scroll->verticalScrollBar()->value() > 0);
	adaptive.hide();
	TEST_CHECK(!styleWarning);
	qInstallMessageHandler(nullptr);
	return 0;
}
