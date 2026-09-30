#include "src/ui/settings_dialog.h"
#include "src/ui/app_theme.h"
#include "src/ui/app_translation.h"
#include "src/ui/audio_device_test_controller.h"
#include "src/core/session_shutdown_service.h"
#include "src/telemetry/diagnostic_pipeline.h"
#include "tests/support/test_check.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtGui/QImageReader>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStackedWidget>
#include <QtPlugin>

#include <nlohmann/json.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <source_location>
#include <thread>

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin)
Q_IMPORT_PLUGIN(QSvgPlugin)
Q_IMPORT_PLUGIN(QSvgIconPlugin)
Q_IMPORT_PLUGIN(QJpegPlugin)
Q_IMPORT_PLUGIN(QGifPlugin)
Q_IMPORT_PLUGIN(QICOPlugin)

namespace OpenMeeting {
class SessionManagerTestAccess final {
public:
	using Owner = std::unique_ptr<SessionManager, void (*)(SessionManager *)>;
	static Owner create(const QString &path) {
		return Owner(new SessionManager(std::make_unique<QSettings>(path, QSettings::IniFormat)),
			[](SessionManager *session) { delete session; });
	}
};
}

namespace MeetingUI {
class SettingsDialogTestAccess final {
public:
	static void use(SettingsDialog &dialog, CameraDeviceDiscovery &discovery) {
		dialog._cameraDiscovery = &discovery;
	}
	static bool ready(const SettingsDialog &dialog) { return dialog._videoDevicesReady; }
	static bool painted(const SettingsDialog &dialog) { return dialog._firstPaintReported; }
	static void request(SettingsDialog &dialog, bool force) { dialog.startDeviceDiscovery(force); }
	static QComboBox *camera(SettingsDialog &dialog) { return dialog._cameraCombo; }
	static QScrollArea *page(SettingsDialog &dialog) {
		return qobject_cast<QScrollArea *>(dialog._pages->currentWidget());
	}
	static QWidget *lastVideoControl(SettingsDialog &dialog) { return dialog._remoteMirror; }
};

// This target exercises the real settings UI and camera-discovery path with
// controlled slow/failing devices. Audio remains pending so preference edits
// also test that saved audio IDs survive; the app links the real controller.
struct AudioDeviceTestController::Impl {};
AudioDeviceTestController::AudioDeviceTestController(QObject *parent) : QObject(parent) {}
AudioDeviceTestController::~AudioDeviceTestController() = default;
void AudioDeviceTestController::refreshDevices() {}
void AudioDeviceTestController::startMicrophoneTest(const QString &) {}
void AudioDeviceTestController::stopMicrophoneTest() {}
void AudioDeviceTestController::playSpeakerTestTone(const QString &, int) {}
void AudioDeviceTestController::stopSpeakerTestTone() {}
void AudioDeviceTestController::stopAll() {}
}

namespace {
using Discovery = MeetingUI::CameraDeviceDiscovery;
using Dialog = MeetingUI::SettingsDialog;
using Access = MeetingUI::SettingsDialogTestAccess;

template <typename Predicate> void waitFor(Predicate predicate,
		std::source_location location = std::source_location::current()) {
	QElapsedTimer timeout;
	timeout.start();
	while (!predicate() && timeout.elapsed() < 5000) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
		QThread::msleep(1);
	}
	if (!predicate()) {
		std::fprintf(stderr, "Timed out waiting at %s:%u\n", location.file_name(), location.line());
		for (auto *widget : QApplication::topLevelWidgets()) {
			std::fprintf(stderr, "Window %s visible=%d exposed=%d\n",
				widget->objectName().toUtf8().constData(), widget->isVisible(),
				widget->windowHandle() && widget->windowHandle()->isExposed());
		}
		TEST_CHECK(false);
	}
}

Discovery::Devices devices(const std::string &id = "camera-a") {
	livekit::DShowCapability capability;
	capability.width = 1280;
	capability.height = 720;
	capability.max_fps = 30;
	return {{"Synthetic camera", id, true, {capability}},
		{"Other camera", "camera-b", false, {capability}}};
}

auto makeSession(const QString &path) {
	auto session = OpenMeeting::SessionManagerTestAccess::create(path);
	auto prefs = session->mediaPreferences();
	prefs.cameraDeviceId = "camera-a";
	prefs.videoCaptureWidth = 1280;
	prefs.videoCaptureHeight = 720;
	prefs.videoCaptureFps = 30;
	prefs.microphoneDeviceId = "saved-microphone";
	prefs.speakerDeviceId = "saved-speaker";
	session->setMediaPreferences(prefs);
	return session;
}

void checkSavedDevices(const OpenMeeting::MediaPreferences &prefs, const QString &camera) {
	TEST_CHECK(prefs.cameraDeviceId == camera);
	TEST_CHECK(prefs.videoCaptureWidth == 1280 && prefs.videoCaptureHeight == 720);
	TEST_CHECK(prefs.videoCaptureFps == 30);
	TEST_CHECK(prefs.microphoneDeviceId == "saved-microphone");
	TEST_CHECK(prefs.speakerDeviceId == "saved-speaker");
}

void benchmarkThemedOpen(const QString &path) {
	auto session = makeSession(path);
	Discovery discovery([] { return devices(); }, [](auto job) { job(); return true; });
	QWidget parent;
	parent.resize(1100, 700);
	QPushButton openSettings(QStringLiteral("Open settings"), &parent);
	openSettings.resize(150, 36);
	parent.show();
	QCoreApplication::processEvents();
	for (int sample = 0; sample != 4; ++sample) {
		QElapsedTimer timer;
		const auto connection = QObject::connect(&openSettings, &QPushButton::clicked, &parent, [&] {
			Dialog dialog(*session, &parent);
			const auto constructed = timer.nsecsElapsed();
			Access::use(dialog, discovery);
			dialog.show();
			const auto shown = timer.nsecsElapsed();
			waitFor([&] { return Access::painted(dialog); });
			const auto painted = timer.nsecsElapsed();
			auto *joinCamera = dialog.findChild<QCheckBox *>("settingsJoinCamera");
			TEST_CHECK(joinCamera && joinCamera->isVisible() && joinCamera->isEnabled());
			const bool previous = session->mediaPreferences().enableVideo;
			joinCamera->click();
			TEST_CHECK(session->mediaPreferences().enableVideo != previous);
			const auto interactive = timer.nsecsElapsed();
			checkSavedDevices(session->mediaPreferences(), "camera-a");
			std::printf("settings_benchmark sample=%d construct_ms=%.3f show_ms=%.3f click_to_paint_ms=%.3f click_to_interactive_ms=%.3f\n",
				sample, constructed / 1e6, (shown - constructed) / 1e6,
				painted / 1e6, interactive / 1e6);
			std::fflush(stdout);
			dialog.reject();
		});
		// Exercise real Qt button signals without including OS input dispatch.
		timer.start();
		openSettings.click();
		QObject::disconnect(connection);
	}
}

void probeEmbeddedImageReader() {
	const auto path = QStringLiteral(":/qt-project.org/styles/commonstyle/images/media-volume-16.png");
	for (int sample = 0; sample != 3; ++sample) {
		for (const bool explicitPng : {false, true}) {
			QElapsedTimer timer;
			timer.start();
			QImageReader reader(path);
			if (explicitPng) {
				reader.setFormat("png");
				reader.setAutoDetectImageFormat(false);
			}
			const auto image = reader.read();
			const auto elapsed = timer.nsecsElapsed();
			TEST_CHECK(!image.isNull());
			std::printf("settings_image_probe sample=%d reader=%s elapsed_ms=%.3f\n",
				sample, explicitPng ? "explicit_png" : "default", elapsed / 1e6);
		}
	}
	std::fflush(stdout);
}

void smallWindowPagesRemainReachable(const QString &path) {
	auto session = makeSession(path);
	// Leave discovery pending while visiting Video; no device is opened.
	std::vector<std::function<void()>> jobs;
	Discovery discovery([] { return devices(); }, [&](auto job) {
		jobs.push_back(std::move(job));
		return true;
	});
	Dialog dialog(*session);
	Access::use(dialog, discovery);
	dialog.show();
	waitFor([&] { return Access::painted(dialog); });
	dialog.resize(400, 320);
	auto *outer = dialog.findChild<QScrollArea *>("adaptiveDialogScroll");
	TEST_CHECK(outer);
	for (auto page : {Dialog::Page::General, Dialog::Page::Video,
			Dialog::Page::Audio, Dialog::Page::About}) {
		dialog.showPage(page);
		QCoreApplication::processEvents();
		auto *scroll = Access::page(dialog);
		TEST_CHECK(scroll && scroll->widget() && scroll->widget()->layout());
		QWidget *target = nullptr;
		switch (page) {
		case Dialog::Page::General: target = dialog.findChild<QCheckBox *>("settingsStayWhenLocked"); break;
		case Dialog::Page::Video: target = Access::lastVideoControl(dialog); break;
		case Dialog::Page::Audio: target = dialog.findChild<QCheckBox *>("autoGainControlCheckBox"); break;
		case Dialog::Page::About: target = scroll->findChild<QPushButton *>("secondaryButton"); break;
		}
		TEST_CHECK(target && target->isVisible());
		scroll->ensureWidgetVisible(target);
		outer->ensureWidgetVisible(target);
		QCoreApplication::processEvents();
		TEST_CHECK(scroll->viewport()->rect().contains(
			target->mapTo(scroll->viewport(), target->rect().center())));
		TEST_CHECK(dialog.rect().contains(target->mapTo(&dialog, target->rect().center())));
	}
	dialog.reject();
}

void firstPaintAndCloseDoNotWaitForDriver(const QString &path) {
	auto session = makeSession(path);
	OpenMeeting::SessionShutdownService worker;
	std::promise<void> release;
	const auto released = release.get_future().share();
	std::atomic<int> scans = 0;
	const auto uiThread = std::this_thread::get_id();
	Discovery discovery([&] {
		TEST_CHECK(std::this_thread::get_id() != uiThread);
		++scans;
		TEST_CHECK(released.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
		return devices();
	}, [&](auto job) { return worker.Submit(std::move(job)); });
	auto dialog = std::make_unique<Dialog>(*session, nullptr,
		Discovery::Clock::now() - std::chrono::milliseconds(123));
	Access::use(*dialog, discovery);
	TEST_CHECK(scans == 0);
	dialog->show();
	waitFor([&] { return Access::painted(*dialog) && scans == 1; });
	TEST_CHECK(dialog->isVisible() && !Access::ready(*dialog));
	bool heartbeat = false;
	QTimer::singleShot(0, dialog.get(), [&] { heartbeat = true; });
	waitFor([&] { return heartbeat; });
	auto *joinCamera = dialog->findChild<QCheckBox *>("settingsJoinCamera");
	TEST_CHECK(joinCamera);
	const bool previous = session->mediaPreferences().enableVideo;
	joinCamera->click();
	TEST_CHECK(session->mediaPreferences().enableVideo != previous);
	checkSavedDevices(session->mediaPreferences(), "camera-a");

	// Close and destroy the receiver while native enumeration is still blocked.
	dialog->reject();
	TEST_CHECK(!dialog->isVisible());
	dialog.reset();
	int changedAfterClose = 0;
	const auto connection = QObject::connect(session.get(), &OpenMeeting::SessionManager::preferencesChanged,
		[&] { ++changedAfterClose; });
	Dialog next(*session);
	Access::use(next, discovery);
	next.show();
	waitFor([&] { return Access::painted(next); });
	TEST_CHECK(scans == 1 && !Access::ready(next)); // joins the existing scan
	auto latest = session->mediaPreferences();
	latest.cameraDeviceId = "camera-b";
	session->setMediaPreferences(latest);
	const auto changesBeforeCompletion = changedAfterClose;
	release.set_value();
	waitFor([&] { return Access::ready(next) && !worker.busy(); });
	TEST_CHECK(changedAfterClose == changesBeforeCompletion);
	checkSavedDevices(next.preferences(), "camera-b"); // uses current, not captured preferences
	TEST_CHECK(Access::camera(next)->isEnabled());
	next.reject();
	QObject::disconnect(connection);

	Dialog cached(*session);
	Access::use(cached, discovery);
	cached.show();
	waitFor([&] { return Access::ready(cached); });
	TEST_CHECK(scans == 1);
	checkSavedDevices(cached.preferences(), "camera-b");
	cached.refreshDevices();
	waitFor([&] { return Access::ready(cached) && !worker.busy(); });
	TEST_CHECK(scans == 2); // explicit refresh bypasses the cache
	cached.reject();
}

void staleQueuedCompletionAndFailurePreservePreferences(const QString &path) {
	auto session = makeSession(path);
	std::vector<std::function<void()>> jobs;
	bool fail = false;
	Discovery discovery([&] {
		if (fail) throw std::runtime_error("synthetic driver failure");
		return devices();
	}, [&](auto job) { jobs.push_back(std::move(job)); return true; });
	Dialog dialog(*session);
	Access::use(dialog, discovery);
	Access::request(dialog, false);
	TEST_CHECK(jobs.size() == 1);
	jobs[0](); // successful result is queued to Qt, but not yet delivered
	fail = true;
	Access::request(dialog, true); // revoke the already-queued result
	QCoreApplication::processEvents();
	TEST_CHECK(!Access::ready(dialog));
	TEST_CHECK(jobs.size() == 2);
	jobs[1]();
	QCoreApplication::processEvents();
	TEST_CHECK(!Access::ready(dialog) && !Access::camera(dialog)->isEnabled());
	dialog.findChild<QCheckBox *>("settingsJoinCamera")->click();
	checkSavedDevices(session->mediaPreferences(), "camera-a");
	fail = false;
	Access::request(dialog, false); // failures are not cached
	TEST_CHECK(jobs.size() == 3);
	jobs[2]();
	waitFor([&] { return Access::ready(dialog); });
	checkSavedDevices(dialog.preferences(), "camera-a");
}

void cacheExpiryEmptyAndSubmissionFailure() {
	int scans = 0;
	auto now = Discovery::Clock::time_point{};
	Discovery discovery([&] { ++scans; return Discovery::Devices{}; },
		[](auto job) { job(); return true; }, [&] { return now; });
	Discovery::Result last;
	auto request = discovery.request([&](const auto &value) { last = value; });
	TEST_CHECK(last.succeeded && last.devices->empty() && scans == 1 && !last.cacheHit);
	now += std::chrono::seconds(29);
	request = discovery.request([&](const auto &value) { last = value; });
	TEST_CHECK(last.succeeded && last.cacheHit && scans == 1);
	now += std::chrono::seconds(1);
	request = discovery.request([&](const auto &value) { last = value; });
	TEST_CHECK(last.succeeded && !last.cacheHit && scans == 2);
	Discovery rejected([] { return devices(); }, [](auto) { return false; });
	request = rejected.request([&](const auto &value) { last = value; });
	TEST_CHECK(!last.succeeded);
	bool accepting = false;
	Discovery retry([] { return devices(); }, [&](auto job) {
		if (!accepting) throw std::runtime_error("executor unavailable");
		job(); return true;
	});
	request = retry.request([&](const auto &value) { last = value; });
	TEST_CHECK(!last.succeeded);
	accepting = true;
	request = retry.request([&](const auto &value) { last = value; });
	TEST_CHECK(last.succeeded && !last.cacheHit);
}

void verifyRecordedMetrics(const std::filesystem::path &root) {
	bool firstPaint = false, probe = false, cacheHit = false, failure = false, audio = false;
	for (const auto &file : std::filesystem::recursive_directory_iterator(root)) {
		if (file.path().extension() != ".jsonl") continue;
		std::ifstream input(file.path());
		std::string line;
		while (std::getline(input, line)) {
			const auto event = nlohmann::json::parse(line);
			if (event["event_name"] == "settings.first_paint") {
				TEST_CHECK(event["attributes"]["measurement_point"] == "click_to_first_qt_paint_completed");
				firstPaint |= event["duration_ms"].get<int>() >= 123;
			} else if (event["event_name"] == "settings.device_probe") {
				const auto &attributes = event["attributes"];
				TEST_CHECK(event.contains("duration_ms"));
				TEST_CHECK(!attributes.contains("device_id") && !attributes.contains("device_name"));
				if (attributes["cache_hit"] == true) {
					TEST_CHECK(event["duration_ms"] == 0);
					TEST_CHECK(attributes["measurement_point"] == "cached_snapshot");
					cacheHit = true;
				} else {
					TEST_CHECK(attributes["measurement_point"] == "worker_device_enumeration");
					probe = true;
				}
				failure |= event["outcome"] == "failure";
				audio |= attributes["media_kind"] == "audio";
			}
		}
	}
	TEST_CHECK(firstPaint && probe && cacheHit && failure && audio);
}
}

int main(int argc, char **argv) {
	QApplication app(argc, argv);
	app.setQuitOnLastWindowClosed(false);
	MeetingUI::AppTranslation::install(app,
		MeetingUI::AppTranslation::startupLocale(app.arguments()));
	MeetingUI::AppTheme::install(app);
	if (app.arguments().contains(QStringLiteral("--image-probe"))) {
		probeEmbeddedImageReader();
		return 0;
	}
	QTemporaryDir temporary;
	TEST_CHECK(temporary.isValid());
	if (app.arguments().contains(QStringLiteral("--benchmark"))) {
		benchmarkThemedOpen(temporary.filePath("benchmark.ini"));
		return 0;
	}
	const auto root = std::filesystem::path(temporary.path().toStdWString()) / "diagnostics";
	auto pipeline = std::make_shared<livekit::diagnostic::DiagnosticPipeline>();
	TEST_CHECK(pipeline->StartWriter(root));
	livekit::diagnostic::InstallBusinessPipeline(pipeline);
	firstPaintAndCloseDoNotWaitForDriver(temporary.filePath("first.ini"));
	smallWindowPagesRemainReachable(temporary.filePath("layout.ini"));
	staleQueuedCompletionAndFailurePreservePreferences(temporary.filePath("stale.ini"));
	cacheExpiryEmptyAndSubmissionFailure();
	using namespace livekit::diagnostic;
	TEST_CHECK(EmitBusinessEvent(Event::SettingsProbe(MediaKind::Audio, 7, Outcome::Success, 2)));
	InstallBusinessPipeline({});
	TEST_CHECK(pipeline->Close() == DrainResult::Completed);
	verifyRecordedMetrics(root);
	std::puts("Settings async, cache, cancellation, preferences and diagnostic persistence PASS");
}
