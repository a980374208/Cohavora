#include "base/basic_types.h"
#include "crl/crl.h"
#include "rpl/rpl.h"
#include "src/media/camera_source_manager.h"
#include "src/net/session_manager.h"
#include "src/ui/camera_switch_completion_owner.h"
#include "src/ui/meeting_room_window.h"
#include "src/ui/meeting_ui_integration.h"
#include "tests/support/test_check.h"
#include "ui/integration.h"
#include "ui/style/style_core.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtPlugin>
#include <QtWidgets/QApplication>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin)
Q_IMPORT_PLUGIN(QSvgPlugin)
Q_IMPORT_PLUGIN(QSvgIconPlugin)
Q_IMPORT_PLUGIN(QJpegPlugin)
Q_IMPORT_PLUGIN(QGifPlugin)
Q_IMPORT_PLUGIN(QICOPlugin)

namespace crl {
rpl::producer<> on_main_update_requests() {
	return rpl::never<>();
}
} // namespace crl

namespace OpenMeeting {

class SessionManagerTestAccess final {
public:
	struct Deleter final {
		void operator()(SessionManager *session) const {
			SessionManagerTestAccess::destroy(session);
		}
	};
	using ScopedSession = std::unique_ptr<SessionManager, Deleter>;

	static ScopedSession create(std::unique_ptr<QSettings> settings) {
		return ScopedSession(new SessionManager(std::move(settings)));
	}

private:
	static void destroy(SessionManager *session) {
		delete session;
	}
};

} // namespace OpenMeeting

class CameraOwnerTestAccess final {
public:
	using Task = std::function<void()>;

	static std::unique_ptr<MeetingUI::MeetingRoomWindow> createWindow(
			std::shared_ptr<livekit::CameraSourceManager> manager,
			OpenMeeting::SessionManager &session) {
		MeetingUI::MeetingRoomWindow::Config config;
		config.displayName = QStringLiteral("Camera Owner Test");
		config.videoEnabled = true;
		return std::unique_ptr<MeetingUI::MeetingRoomWindow>(
			new MeetingUI::MeetingRoomWindow(
				MeetingUI::MeetingRoomWindow::CameraOwnerTestTag{},
				config,
				std::move(manager),
				session));
	}

	static void setEffects(
			MeetingUI::MeetingRoomWindow &window,
			MeetingUI::MeetingRoomWindow::CameraLogEffect log,
			MeetingUI::MeetingRoomWindow::CameraWarningEffect warning) {
		window._cameraLogEffect = std::move(log);
		window._cameraWarningEffect = std::move(warning);
	}

	static void request(MeetingUI::MeetingRoomWindow &window, const QString &path) {
		window._bottomBar->_videoDeviceStream.fire_copy(path);
	}

	static void stop(MeetingUI::MeetingRoomWindow &window) {
		window.stopLiveKitSession();
	}

	static uint64_t enqueueCount(const MeetingUI::MeetingRoomWindow &window) {
		return window._cameraCompletionOwner->enqueueCountForTest();
	}

	static bool ownerClosed(const MeetingUI::MeetingRoomWindow &window) {
		return window._cameraCompletionOwner->closedForTest();
	}

	static void setManagerHooks(
			livekit::CameraSourceManager &manager,
			std::function<void(int, Task)> timeoutScheduler,
			std::function<void(Task)> cleanupScheduler,
			Task beforeDelivery = {}) {
		manager.timeout_scheduler_for_test_ = std::move(timeoutScheduler);
		manager.cleanup_scheduler_for_test_ = std::move(cleanupScheduler);
		manager.before_terminal_delivery_for_test_ = std::move(beforeDelivery);
	}

	static void setBeforeDelivery(
			livekit::CameraSourceManager &manager,
			Task beforeDelivery) {
		manager.before_terminal_delivery_for_test_ = std::move(beforeDelivery);
	}
};

namespace {

using namespace std::chrono_literals;

struct Effects final {
	int requestLogs = 0;
	int resultLogs = 0;
	int warnings = 0;
	std::thread::id lastResultThread;
};

class ControlledCapturer final : public livekit::ICameraCapturer {
public:
	explicit ControlledCapturer(bool initOk = true, bool startOk = true)
	:	_initOk(initOk),
		_startOk(startOk) {
	}

	~ControlledCapturer() override {
		Stop();
	}

	bool Init(
			const livekit::DShowCaptureConfig &config,
			std::shared_ptr<livekit::VideoSource> source) override {
		_config = config;
		_source = std::move(source);
		return _initOk;
	}

	bool Start() override {
		_running.store(_startOk);
		return _startOk;
	}

	void Stop() override {
		_running.store(false);
		_stopCount.fetch_add(1);
	}

	bool IsRunning() const noexcept override {
		return _running.load();
	}

	livekit::DShowCaptureConfig GetConfig() const override {
		return _config;
	}

	std::string GetDevicePath() const override {
		return _config.device_path;
	}

	void produceFrame() {
		TEST_CHECK(_running.load());
		TEST_CHECK(_source != nullptr);
		auto frame = livekit::VideoFrame::create(8, 8, livekit::VideoBufferType::RGBA);
		livekit::VideoCaptureOptions options;
		options.timestamp_us = 1;
		_source->captureFrame(frame, options);
	}

	int stopCount() const {
		return _stopCount.load();
	}

private:
	bool _initOk = true;
	bool _startOk = true;
	std::atomic<bool> _running{false};
	std::atomic<int> _stopCount{0};
	livekit::DShowCaptureConfig _config;
	std::shared_ptr<livekit::VideoSource> _source;
};

std::unique_ptr<QSettings> makeSettings(const QString &directory) {
	auto settings = std::make_unique<QSettings>(
		directory + QStringLiteral("/camera-owner.ini"),
		QSettings::IniFormat);
	settings->setFallbacksEnabled(false);
	settings->setValue("server/baseUrl", "http://127.0.0.1:1");
	settings->sync();
	TEST_CHECK(settings->status() == QSettings::NoError);
	return settings;
}

void drainEvents() {
	for (int i = 0; i != 8; ++i) {
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
	}
}

struct Fixture final {
	QTemporaryDir settingsDirectory;
	OpenMeeting::SessionManagerTestAccess::ScopedSession session;
	std::vector<std::shared_ptr<ControlledCapturer>> captures;
	std::vector<CameraOwnerTestAccess::Task> timeouts;
	int cleanupRuns = 0;
	std::shared_ptr<livekit::CameraSourceManager> manager;
	std::shared_ptr<Effects> effects = std::make_shared<Effects>();
	std::unique_ptr<MeetingUI::MeetingRoomWindow> window;

	Fixture()
	:	session(OpenMeeting::SessionManagerTestAccess::create(
			makeSettings(settingsDirectory.path()))) {
		TEST_CHECK(settingsDirectory.isValid());
		session->loginAsGuest(QStringLiteral("Camera Test"), QStringLiteral("camera-test"));
		auto factory = [this]() -> std::shared_ptr<livekit::ICameraCapturer> {
			auto capture = std::make_shared<ControlledCapturer>();
			captures.push_back(capture);
			return capture;
		};
		manager = livekit::CameraSourceManager::Create(
			std::make_shared<livekit::VideoSource>(8, 8),
			std::move(factory));
		livekit::DShowCaptureConfig config;
		config.device_path = "camera-a";
		config.width = 8;
		config.height = 8;
		TEST_CHECK(manager->Start(config));
		CameraOwnerTestAccess::setManagerHooks(
			*manager,
			[this](int, CameraOwnerTestAccess::Task task) {
				timeouts.push_back(std::move(task));
			},
			[this](CameraOwnerTestAccess::Task task) {
				++cleanupRuns;
				task();
			});
		window = CameraOwnerTestAccess::createWindow(manager, *session);
		CameraOwnerTestAccess::setEffects(
			*window,
			[effects = effects](bool error, const QString &, const QString &) {
				if (error) {
					++effects->resultLogs;
					effects->lastResultThread = std::this_thread::get_id();
				} else {
					++effects->requestLogs;
				}
			},
			[effects = effects](QWidget *, const QString &, const QString &) {
				++effects->warnings;
			});
	}

	~Fixture() {
		window.reset();
		manager->Stop();
		session->logout(false);
		drainEvents();
	}

	CameraOwnerTestAccess::Task takeTimeout(size_t index = 0) {
		TEST_CHECK(timeouts.size() > index);
		return std::move(timeouts[index]);
	}
};

int executed = 0;

template <typename Callback>
void runCase(const char *name, Callback &&callback) {
	std::cout << "[CPPQT-002] " << name << std::endl;
	callback();
	++executed;
}

void verifyAliveTimeout() {
	runCase("A alive timeout reaches GUI once", [] {
		Fixture fixture;
		CameraOwnerTestAccess::request(*fixture.window, QStringLiteral("camera-b"));
		TEST_CHECK(fixture.timeouts.size() == 1);
		auto timeout = fixture.takeTimeout();
		std::thread worker([&] { timeout(); });
		worker.join();
		drainEvents();
		TEST_CHECK(fixture.effects->requestLogs == 1);
		TEST_CHECK(fixture.effects->resultLogs == 1);
		TEST_CHECK(fixture.effects->warnings == 1);
		TEST_CHECK(fixture.effects->lastResultThread == std::this_thread::get_id());
		TEST_CHECK(fixture.manager->GetSwitchState() == livekit::CameraSwitchState::Aborted);
	});
}

void verifyAliveFirstFrame() {
	runCase("A first frame success reaches GUI once", [] {
		Fixture fixture;
		CameraOwnerTestAccess::request(*fixture.window, QStringLiteral("camera-b"));
		TEST_CHECK(fixture.captures.size() == 2);
		std::thread producer([capture = fixture.captures[1]] { capture->produceFrame(); });
		producer.join();
		drainEvents();
		TEST_CHECK(fixture.effects->requestLogs == 2);
		TEST_CHECK(fixture.effects->resultLogs == 0);
		TEST_CHECK(fixture.effects->warnings == 0);
		TEST_CHECK(fixture.manager->GetSwitchState() == livekit::CameraSwitchState::Committed);
	});
}

void verifyImmediateCompletionOutsideManagerLock() {
	runCase("immediate completion runs outside manager mutex", [] {
		Fixture fixture;
		std::atomic<int> hookCalls{0};
		CameraOwnerTestAccess::setBeforeDelivery(*fixture.manager, [&] {
			(void)fixture.manager->GetSwitchState();
			hookCalls.fetch_add(1);
		});
		CameraOwnerTestAccess::request(*fixture.window, QStringLiteral("camera-a"));
		drainEvents();
		TEST_CHECK(hookCalls.load() == 1);
		TEST_CHECK(CameraOwnerTestAccess::enqueueCount(*fixture.window) == 1);
	});
}

void verifyDeleteAfterTerminalSelection() {
	runCase("B timeout selected then Window deleted", [] {
		Fixture fixture;
		std::mutex mutex;
		std::condition_variable cv;
		bool selected = false;
		bool resume = false;
		std::atomic<bool> callbackReturned{false};
		CameraOwnerTestAccess::setBeforeDelivery(*fixture.manager, [&] {
			std::unique_lock<std::mutex> lock(mutex);
			selected = true;
			cv.notify_one();
			TEST_CHECK(cv.wait_for(lock, 2s, [&] { return resume; }));
		});
		CameraOwnerTestAccess::request(*fixture.window, QStringLiteral("camera-b"));
		auto timeout = fixture.takeTimeout();
		std::thread worker([&] {
			timeout();
			callbackReturned.store(true);
		});
		{
			std::unique_lock<std::mutex> lock(mutex);
			TEST_CHECK(cv.wait_for(lock, 2s, [&] { return selected; }));
		}
		fixture.effects->requestLogs = 0;
		fixture.window.reset();
		TEST_CHECK(fixture.captures.front()->stopCount() > 0);
		{
			std::lock_guard<std::mutex> lock(mutex);
			resume = true;
		}
		cv.notify_one();
		worker.join();
		drainEvents();
		TEST_CHECK(callbackReturned.load());
		TEST_CHECK(fixture.effects->resultLogs == 0);
		TEST_CHECK(fixture.effects->warnings == 0);
	});
}

void verifyDeleteAfterQueue() {
	runCase("B queued result deleted before GUI drain", [] {
		Fixture fixture;
		CameraOwnerTestAccess::request(*fixture.window, QStringLiteral("camera-b"));
		auto timeout = fixture.takeTimeout();
		std::thread worker([&] { timeout(); });
		worker.join();
		TEST_CHECK(CameraOwnerTestAccess::enqueueCount(*fixture.window) == 1);
		fixture.effects->requestLogs = 0;
		fixture.window.reset();
		drainEvents();
		TEST_CHECK(fixture.effects->resultLogs == 0);
		TEST_CHECK(fixture.effects->warnings == 0);
	});
}

void verifyOldWindowCannotAffectNewWindow() {
	runCase("C old Window completion cannot affect new Window", [] {
		Fixture oldFixture;
		std::mutex mutex;
		std::condition_variable cv;
		bool selected = false;
		bool resume = false;
		CameraOwnerTestAccess::setBeforeDelivery(*oldFixture.manager, [&] {
			std::unique_lock<std::mutex> lock(mutex);
			selected = true;
			cv.notify_one();
			TEST_CHECK(cv.wait_for(lock, 2s, [&] { return resume; }));
		});
		CameraOwnerTestAccess::request(*oldFixture.window, QStringLiteral("old-camera"));
		auto oldTimeout = oldFixture.takeTimeout();
		std::thread oldWorker([&] { oldTimeout(); });
		{
			std::unique_lock<std::mutex> lock(mutex);
			TEST_CHECK(cv.wait_for(lock, 2s, [&] { return selected; }));
		}
		oldFixture.window.reset();

		Fixture newFixture;
		CameraOwnerTestAccess::request(*newFixture.window, QStringLiteral("new-camera"));
		auto newTimeout = newFixture.takeTimeout();
		std::thread newWorker([&] { newTimeout(); });
		newWorker.join();
		drainEvents();
		TEST_CHECK(newFixture.effects->resultLogs == 1);
		TEST_CHECK(newFixture.effects->warnings == 1);

		{
			std::lock_guard<std::mutex> lock(mutex);
			resume = true;
		}
		cv.notify_one();
		oldWorker.join();
		drainEvents();
		TEST_CHECK(oldFixture.effects->resultLogs == 0);
		TEST_CHECK(newFixture.effects->resultLogs == 1);
		TEST_CHECK(newFixture.effects->warnings == 1);
	});
}

void verifyQueuedBeforeSessionInvalidation() {
	runCase("D queued timeout rejected by session invalidation", [] {
		Fixture fixture;
		CameraOwnerTestAccess::request(*fixture.window, QStringLiteral("camera-b"));
		auto timeout = fixture.takeTimeout();
		std::thread worker([&] { timeout(); });
		worker.join();
		TEST_CHECK(CameraOwnerTestAccess::enqueueCount(*fixture.window) == 1);
		fixture.effects->requestLogs = 0;
		fixture.session->invalidateSession(OpenMeeting::SessionInvalidationReason::TokenInvalid);
		TEST_CHECK(CameraOwnerTestAccess::ownerClosed(*fixture.window));
		drainEvents();
		TEST_CHECK(fixture.effects->resultLogs == 0);
		TEST_CHECK(fixture.effects->warnings == 0);
	});
}

void verifySessionInvalidationBeforeTimeout() {
	runCase("D session invalidation rejects later timeout", [] {
		Fixture fixture;
		CameraOwnerTestAccess::request(*fixture.window, QStringLiteral("camera-b"));
		auto timeout = fixture.takeTimeout();
		fixture.effects->requestLogs = 0;
		fixture.session->invalidateSession(OpenMeeting::SessionInvalidationReason::DuplicatedLogin);
		TEST_CHECK(CameraOwnerTestAccess::ownerClosed(*fixture.window));
		std::thread worker([&] { timeout(); });
		worker.join();
		drainEvents();
		TEST_CHECK(fixture.effects->resultLogs == 0);
		TEST_CHECK(fixture.effects->warnings == 0);
	});
}

void verifyNewerRequestWins() {
	runCase("newer request epoch rejects old completion", [] {
		Fixture fixture;
		CameraOwnerTestAccess::request(*fixture.window, QStringLiteral("camera-b"));
		CameraOwnerTestAccess::request(*fixture.window, QStringLiteral("camera-c"));
		TEST_CHECK(fixture.timeouts.size() == 2);
		auto oldTimeout = fixture.takeTimeout(0);
		auto newTimeout = fixture.takeTimeout(1);
		std::thread oldWorker([&] { oldTimeout(); });
		oldWorker.join();
		std::thread newWorker([&] { newTimeout(); });
		newWorker.join();
		drainEvents();
		TEST_CHECK(fixture.effects->resultLogs == 1);
		TEST_CHECK(fixture.effects->warnings == 1);
	});
}

} // namespace

int main(int argc, char *argv[]) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
	QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
	QStandardPaths::setTestModeEnabled(true);
	crl::details::init();
	QApplication app(argc, argv);
	app.setApplicationName(QStringLiteral("CPPQT002CameraOwnerTest"));
	MeetingUI::MeetingUiIntegration integration;
	Ui::Integration::Set(&integration);
	style::StartManager(100);

	verifyAliveTimeout();
	verifyAliveFirstFrame();
	verifyImmediateCompletionOutsideManagerLock();
	verifyDeleteAfterTerminalSelection();
	verifyDeleteAfterQueue();
	verifyOldWindowCannotAffectNewWindow();
	verifyQueuedBeforeSessionInvalidation();
	verifySessionInvalidationBeforeTimeout();
	verifyNewerRequestWins();

	style::StopManager();
	TEST_CHECK(executed == 9);
	std::cout << "CPPQT002_CASES_PLANNED=9 EXECUTED=" << executed
	          << " PASSED=" << executed << " FAILED=0" << std::endl;
	return 0;
}
