#include "src/ui/meeting_room_window.h"
#include "src/ui/app_theme.h"
#include "src/ui/app_translation.h"
#include "src/ui/meeting_ui_integration.h"
#include "tests/support/test_check.h"
#include "crl/crl.h"
#include "rpl/never.h"
#include "ui/style/style_core.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>
#include <QtWidgets/QApplication>
#include <QtPlugin>
#include <future>
#include <cstdio>

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin)
Q_IMPORT_PLUGIN(QSvgPlugin)
Q_IMPORT_PLUGIN(QSvgIconPlugin)
Q_IMPORT_PLUGIN(QJpegPlugin)
Q_IMPORT_PLUGIN(QGifPlugin)
Q_IMPORT_PLUGIN(QICOPlugin)

namespace crl { rpl::producer<> on_main_update_requests() { return rpl::never<>(); } }

class ParticipantWindowTestAccess final {
public:
	static void discovery(MeetingUI::RoomBottomBarWidget &bar,
			MeetingUI::CameraDeviceDiscovery &camera, MeetingUI::AudioDeviceDiscovery &microphone,
			MeetingUI::AudioDeviceDiscovery &speaker) {
		bar._cameraDiscovery = &camera;
		bar._microphoneDiscovery = &microphone;
		bar._speakerDiscovery = &speaker;
	}
	static QMenu *menu(MeetingUI::RoomBottomBarWidget &bar) { return bar._deviceMenu; }
	static void camera(MeetingUI::RoomBottomBarWidget &bar) { bar.toggleVideo(); }
	static void microphone(MeetingUI::RoomBottomBarWidget &bar) { bar.toggleAudio(); }
	static void speaker(MeetingUI::RoomBottomBarWidget &bar) { bar.toggleSpeaker(); }
};

namespace {
using Access = ParticipantWindowTestAccess;
template <typename Predicate> void waitFor(Predicate predicate) {
	QElapsedTimer timeout;
	timeout.start();
	while (!predicate() && timeout.elapsed() < 5000) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
		QThread::msleep(1);
	}
	TEST_CHECK(predicate());
}

QAction *deviceAction(QMenu *menu, const QString &id) {
	if (menu) for (auto *action : menu->actions()) {
		if (action->isCheckable() && action->data().toString() == id) return action;
	}
	return nullptr;
}

void deviceMenusAndEnableActions() {
	using namespace MeetingUI;
	OpenMeeting::SessionShutdownService worker;
	const auto uiThread = std::this_thread::get_id();
	std::promise<void> release;
	const auto released = release.get_future().share();
	std::atomic<int> cameraScans = 0, microphoneScans = 0, speakerScans = 0;
	const auto submit = [&](auto job) { return worker.Submit(std::move(job)); };
	CameraDeviceDiscovery camera([&] {
		TEST_CHECK(std::this_thread::get_id() != uiThread);
		++cameraScans;
		TEST_CHECK(released.wait_for(std::chrono::seconds(15)) == std::future_status::ready);
		return CameraDeviceDiscovery::Devices{{"Camera", "camera-a", true, {}}};
	}, submit);
	AudioDeviceDiscovery microphone([&] {
		TEST_CHECK(std::this_thread::get_id() != uiThread);
		++microphoneScans;
		// Preserve GetDefaultInputDevice's fallback when no default is marked.
		return AudioDeviceDiscovery::Devices{{"microphone-a", "Microphone", false}};
	}, submit);
	AudioDeviceDiscovery speaker([&] {
		TEST_CHECK(std::this_thread::get_id() != uiThread);
		++speakerScans;
		return AudioDeviceDiscovery::Devices{{"speaker-a", "Speaker", true}};
	}, submit);
	RoomBottomBarWidget bar;
	Access::discovery(bar, camera, microphone, speaker);
	bar.resize(1100, 72);
	bar.show();
	QCoreApplication::processEvents();
	const auto position = bar.mapToGlobal(QPoint(50, 5));
	int cameraActions = 0, microphoneActions = 0, speakerActions = 0;
	QString cameraSelection, microphoneSelection, speakerSelection;
	rpl::lifetime lifetime;
	bar.toggleVideoRequested() | rpl::on_next([&](bool) { ++cameraActions; }, lifetime);
	bar.toggleAudioRequested() | rpl::on_next([&](bool) { ++microphoneActions; }, lifetime);
	bar.toggleSpeakerRequested() | rpl::on_next([&](bool) { ++speakerActions; }, lifetime);
	bar.videoDeviceChanged() | rpl::on_next([&](QString id) { cameraSelection = id; }, lifetime);
	bar.microphoneDeviceChanged() | rpl::on_next([&](QString id) { microphoneSelection = id; }, lifetime);
	bar.speakerDeviceChanged() | rpl::on_next([&](QString id) { speakerSelection = id; }, lifetime);

	QElapsedTimer opened;
	opened.start();
	bar.showVideoDeviceMenu(position);
	TEST_CHECK(Access::menu(bar) && Access::menu(bar)->isVisible());
	const auto popupMs = opened.nsecsElapsed() / 1e6;
	waitFor([&] { return cameraScans == 1; });
	bool heartbeat = false;
	QTimer::singleShot(0, &bar, [&] { heartbeat = true; });
	waitFor([&] { return heartbeat; });
	TEST_CHECK(!deviceAction(Access::menu(bar), "camera-a"));
	QPointer<QMenu> oldMenu = Access::menu(bar);
	oldMenu->close();
	bar.showVideoDeviceMenu(position);
	QCoreApplication::processEvents();
	TEST_CHECK(cameraScans == 1); // reopening joins the blocked native scan
	TEST_CHECK(Access::menu(bar) != oldMenu);
	release.set_value();
	waitFor([&] { return deviceAction(Access::menu(bar), "camera-a") != nullptr; });
	deviceAction(Access::menu(bar), "camera-a")->trigger();
	TEST_CHECK(cameraSelection == "camera-a");
	Access::menu(bar)->close();
	bar.showVideoDeviceMenu(position);
	waitFor([&] { return deviceAction(Access::menu(bar), "camera-a") != nullptr; });
	TEST_CHECK(cameraScans == 1); // same snapshot as enable preflight
	Access::menu(bar)->close();

	bar.setVideoEnabled(false);
	Access::camera(bar); // queues a cached completion
	bar.setInRecovery(true); // reject it before it reaches Qt
	QCoreApplication::processEvents();
	TEST_CHECK(cameraActions == 0 && !bar.isVideoEnabled());
	bar.setInRecovery(false);
	Access::camera(bar);
	waitFor([&] { return cameraActions == 1; });
	TEST_CHECK(bar.isVideoEnabled() && cameraScans == 1);
	Access::camera(bar); // stopping never waits for discovery
	TEST_CHECK(cameraActions == 2 && !bar.isVideoEnabled());
	Access::camera(bar);
	bar.hide();
	QCoreApplication::processEvents();
	TEST_CHECK(cameraActions == 2 && !bar.isVideoEnabled());
	bar.show();

	bar.showAudioDeviceMenu(position);
	TEST_CHECK(Access::menu(bar)->isVisible());
	waitFor([&] { return deviceAction(Access::menu(bar), "microphone-a") != nullptr; });
	deviceAction(Access::menu(bar), "microphone-a")->trigger();
	TEST_CHECK(microphoneSelection == "microphone-a");
	Access::menu(bar)->close();
	bar.setAudioMuted(true);
	Access::microphone(bar);
	waitFor([&] { return microphoneActions == 1; });
	TEST_CHECK(!bar.isAudioMuted() && microphoneScans == 1);
	Access::microphone(bar);
	TEST_CHECK(microphoneActions == 2 && bar.isAudioMuted());

	bar.showSpeakerDeviceMenu(position);
	TEST_CHECK(Access::menu(bar)->isVisible());
	waitFor([&] { return deviceAction(Access::menu(bar), "speaker-a") != nullptr; });
	deviceAction(Access::menu(bar), "speaker-a")->trigger();
	TEST_CHECK(speakerSelection == "speaker-a");
	Access::menu(bar)->close();
	bar.setSpeakerMuted(true);
	Access::speaker(bar);
	waitFor([&] { return speakerActions == 1; });
	TEST_CHECK(speakerScans == 1);
	Access::speaker(bar);
	TEST_CHECK(speakerActions == 2);

	CameraDeviceDiscovery failed([]() -> CameraDeviceDiscovery::Devices { throw 1; }, submit);
	Access::discovery(bar, failed, microphone, speaker);
	bar.showVideoDeviceMenu(position);
	waitFor([&] {
		for (auto *action : Access::menu(bar)->actions()) {
			if (action->text() == QCoreApplication::translate("MeetingUI", "Failed to list cameras")) return true;
		}
		return false;
	});
	TEST_CHECK(cameraActions == 2);
	bar.hide();
	waitFor([&] { return !worker.busy(); });
	std::printf("MEETING_DEVICE_DISCOVERY PASS popup_before_camera_result_ms=%.3f native_devices=NOT_RUN\n", popupMs);
}
}

int main(int argc, char **argv) {
	crl::details::init();
	QApplication app(argc, argv);
	app.setQuitOnLastWindowClosed(false);
	MeetingUI::MeetingUiIntegration integration;
	Ui::Integration::Set(&integration);
	style::StartManager(100);
	MeetingUI::AppTranslation::install(app, MeetingUI::AppTranslation::startupLocale(app.arguments()));
	MeetingUI::AppTheme::install(app);
	deviceMenusAndEnableActions();
	style::StopManager();
}
