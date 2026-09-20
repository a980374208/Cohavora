// Opt-in L3: two independent Room clients use a real LiveKit service. Only an
// owned, animated test window is captured; no desktop image is saved. Network
// publication, negotiation, decoding and unpublish use production code.
#include <asio.hpp>
#include "room.h"
#include "screen_share_session.h"
#include "render/owned_i420_frame.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

namespace {
using namespace std::chrono_literals;
using State = livekit::ScreenShareState;
using Source = livekit::TrackSource;
struct Failure { const char* code; };
void Require(bool value, const char* code) { if (!value) throw Failure{code}; }

asio::awaitable<void> Delay(std::chrono::milliseconds duration) {
    asio::steady_timer timer(co_await asio::this_coro::executor);
    timer.expires_after(duration);
    co_await timer.async_wait(asio::use_awaitable);
}
template <class Predicate>
asio::awaitable<void> Until(Predicate predicate, const char* code, std::chrono::seconds timeout = 20s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        Require(std::chrono::steady_clock::now() < deadline, code);
        co_await Delay(20ms);
    }
}

class PatternWindow {
public:
    PatternWindow() {
        std::promise<HWND> ready;
        auto result = ready.get_future();
        thread_ = std::thread([ready = std::move(ready)]() mutable {
            WNDCLASSW type{};
            type.lpfnWndProc = Procedure;
            type.hInstance = GetModuleHandleW(nullptr);
            type.lpszClassName = L"LiveKitScreenShareL3";
            RegisterClassW(&type);
            HWND hwnd = CreateWindowExW(0, type.lpszClassName, L"LiveKit screen-share test pattern",
                WS_OVERLAPPEDWINDOW, 80, 80, 800, 600, nullptr, nullptr, type.hInstance, nullptr);
            if (hwnd) {
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                SetTimer(hwnd, 1, 500, nullptr);
                UpdateWindow(hwnd);
            }
            ready.set_value(hwnd);
            if (!hwnd) return;
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        });
        hwnd_ = result.get();
    }
    ~PatternWindow() { Close(); }
    void Close() {
        if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        hwnd_ = nullptr;
        if (thread_.joinable()) thread_.join();
    }
    livekit::DesktopSource source() const {
        Require(hwnd_ != nullptr, "pattern_window_unavailable");
        return {livekit::DesktopSourceKind::Window, reinterpret_cast<intptr_t>(hwnd_), {}};
    }
private:
    static LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        if (message == WM_TIMER) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, !GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            InvalidateRect(hwnd, nullptr, false);
            return 0;
        }
        if (message == WM_PAINT || message == WM_PRINTCLIENT) {
            PAINTSTRUCT paint{};
            HDC dc = message == WM_PAINT ? BeginPaint(hwnd, &paint) : reinterpret_cast<HDC>(wp);
            const bool inverted = GetWindowLongPtrW(hwnd, GWLP_USERDATA) != 0;
            RECT rect{};
            GetClientRect(hwnd, &rect);
            FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(inverted ? BLACK_BRUSH : WHITE_BRUSH)));
            rect.right /= 2;
            FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(inverted ? WHITE_BRUSH : BLACK_BRUSH)));
            if (message == WM_PAINT) EndPaint(hwnd, &paint);
            return 0;
        }
        if (message == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
        if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    HWND hwnd_ = nullptr;
    std::thread thread_;
};

struct CaptureCounts {
    std::atomic<int> frames{0}, stopped{0}, ended{0};
};
class CountedCapture final : public livekit::IDesktopCapture {
public:
    explicit CountedCapture(std::shared_ptr<CaptureCounts> counts)
        : counts_(std::move(counts)), capture_(livekit::CreateDesktopCapture()) {}
    void Start(livekit::DesktopSource source, FrameCallback frame, EndCallback end) override {
        capture_->Start(std::move(source), [counts = counts_, frame = std::move(frame)](const auto& value) {
            ++counts->frames;
            frame(value);
        }, [counts = counts_, end = std::move(end)] { ++counts->ended; end(); });
    }
    void Stop() override { capture_->Stop(); ++counts_->stopped; }
private:
    std::shared_ptr<CaptureCounts> counts_;
    std::unique_ptr<livekit::IDesktopCapture> capture_;
};

struct ReceivedTrack {
    Source source = Source::Unknown;
    std::atomic<int> frames{0}, black_left{0}, white_left{0}, width{0}, height{0};
    std::atomic<bool> unpublished{false}, unsubscribed{false};
    livekit::Track::I420VideoFrameSubscription subscription;
};

class Listener final : public livekit::RoomListener {
public:
    explicit Listener(asio::any_io_executor executor) : executor_(std::move(executor)) {}
    std::weak_ptr<livekit::ScreenShareSession> share;
    std::atomic<bool> stop_on_reconnect{false};
    std::atomic<int> reconnecting{0}, reconnected{0}, republished{0};
    void OnReconnecting() override {
        ++reconnecting;
        asio::post(executor_, [weak = share, stop = stop_on_reconnect.exchange(false)] {
            if (auto session = weak.lock()) {
                session->SetTransportReady(false);
                if (stop) session->Stop();
            }
        });
    }
    void OnReconnected() override {
        ++reconnected;
        asio::post(executor_, [weak = share] { if (auto session = weak.lock()) session->SetTransportReady(true); });
    }
    void OnLocalTrackRepublished(const std::string&, std::shared_ptr<livekit::TrackPublication>) override {
        ++republished;
    }
    void OnTrackSubscribed(std::shared_ptr<livekit::Track> track,
                           std::shared_ptr<livekit::TrackPublication> publication,
                           std::shared_ptr<livekit::RemoteParticipant>) override {
        if (!track || track->kind() != livekit::TrackKind::Video) return;
        std::cout << "[SUBSCRIBED] source=" << int(track->source()) << std::endl;
        auto record = std::make_shared<ReceivedTrack>();
        record->source = track->source();
        const std::weak_ptr<ReceivedTrack> weak = record;
        record->subscription = track->subscribeI420VideoFrames([weak](const auto& frame) {
            auto record = weak.lock();
            if (!record) return;
            ++record->frames;
            record->width = frame->width(); record->height = frame->height();
            if (frame->width() < 160 || frame->height() < 100) return;
            const auto* row = frame->data_y() + (frame->height() / 2) * frame->stride_y();
            const int left = row[frame->width() / 4], right = row[3 * frame->width() / 4];
            if (left < 70 && right > 180) ++record->black_left;
            if (right < 70 && left > 180) ++record->white_left;
        });
        std::lock_guard lock(mutex_);
        tracks_[publication->sid()] = std::move(record);
    }
    void OnTrackUnpublished(std::shared_ptr<livekit::RemoteParticipant>,
                            std::shared_ptr<livekit::TrackPublication> publication) override {
        if (auto record = Find(publication->sid())) record->unpublished = true;
    }
    void OnTrackUnsubscribed(std::shared_ptr<livekit::Track>,
                             std::shared_ptr<livekit::TrackPublication> publication,
                             std::shared_ptr<livekit::RemoteParticipant>) override {
        if (auto record = Find(publication->sid())) record->unsubscribed = true;
    }
    std::shared_ptr<ReceivedTrack> Find(const std::string& sid) {
        std::lock_guard lock(mutex_);
        auto it = tracks_.find(sid);
        return it == tracks_.end() ? nullptr : it->second;
    }
private:
    asio::any_io_executor executor_;
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<ReceivedTrack>> tracks_;
};

struct Peer {
    std::shared_ptr<livekit::Room> room;
    std::shared_ptr<Listener> listener;
    std::shared_ptr<livekit::ScreenShareSession> share;
    std::shared_ptr<CaptureCounts> captures = std::make_shared<CaptureCounts>();
    std::shared_ptr<std::atomic<bool>> cancel_publish = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<int>> publish_calls = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<bool>> camera_running = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<livekit::LocalVideoTrack> camera;
    explicit Peer(asio::any_io_executor executor) {
        room = livekit::Room::Create(executor);
        room->SetLogHandler([](const std::string& category, const std::string& tag, const std::string& message) {
            std::cout << "[NATIVE] " << category << '/' << tag;
            if (tag == "ICE_STATE" || tag == "PC_STATE" || category == "ERROR" || category == "DYNACAST")
                std::cout << ' ' << message;
            std::cout << std::endl;
        });
        listener = std::make_shared<Listener>(executor);
        room->AddListener(listener);
        auto backend = livekit::ScreenShareSession::ForRoom(room);
        backend.capture = [counts = captures] { return std::make_unique<CountedCapture>(counts); };
        auto publish = backend.publish;
        backend.publish = [publish, listener = listener, executor, cancel = cancel_publish, calls = publish_calls]
                (std::shared_ptr<livekit::LocalVideoTrack> track) -> asio::awaitable<void> {
            ++*calls;
            if (cancel->exchange(false)) {
                asio::post(executor, [weak = listener->share] { if (auto session = weak.lock()) session->Stop(); });
            }
            co_await publish(std::move(track));
        };
        share = std::make_shared<livekit::ScreenShareSession>(executor, std::move(backend),
            [](livekit::ScreenShareSnapshot value) {
                if (value.error != livekit::ScreenShareError::None)
                    std::cout << "[SHARE_STATE] state=" << int(value.state) << " error=" << int(value.error) << std::endl;
            });
        listener->share = share;
    }
    std::string LocalSid(Source source) const {
        auto local = room->local_participant();
        if (local) for (const auto& [sid, publication] : local->tracks())
            if (publication && publication->track() && publication->track()->source() == source) return sid;
        return {};
    }
    bool RemoteHas(const std::string& sid) const {
        for (const auto& [id, participant] : room->remote_participants())
            if (participant && participant->get_publication(sid)) return true;
        return false;
    }
    bool RemoteHasSource(const std::string& identity, Source source) const {
        for (const auto& [id, participant] : room->remote_participants()) {
            if (!participant || participant->identity() != identity) continue;
            for (const auto& [sid, publication] : participant->tracks())
                if (publication->track() && publication->track()->source() == source) return true;
        }
        return false;
    }
    asio::awaitable<void> StartCamera() {
        auto source = std::make_shared<livekit::VideoSource>(320, 180);
        camera = livekit::LocalVideoTrack::createLocalVideoTrack("screen-test-camera", source);
        camera_running->store(true);
        asio::co_spawn(room->executor(), CameraFrames(source, camera_running), asio::detached);
        co_await room->local_participant()->PublishTrackAsync(camera);
    }
    static asio::awaitable<void> CameraFrames(std::shared_ptr<livekit::VideoSource> source,
                                             std::shared_ptr<std::atomic<bool>> running) {
        auto frame = livekit::VideoFrame::create(320, 180, livekit::VideoBufferType::I420);
        std::fill(frame.data(), frame.data() + frame.dataSize(), uint8_t{128});
        while (running->load()) { source->captureFrame(frame); co_await Delay(66ms); }
    }
    void Shutdown() { share->Shutdown(); camera_running->store(false); room->Disconnect(); }
};

struct ActiveScreen { std::string sid; std::shared_ptr<ReceivedTrack> record; };
asio::awaitable<ActiveScreen> StartScreen(Peer& sender, Peer& receiver, PatternWindow& window) {
    sender.share->Start(window.source());
    co_await Until([&] { return sender.share->snapshot().state == State::Active; }, "screen_publish_not_active");
    const auto sid = sender.LocalSid(Source::ScreenShareVideo);
    Require(!sid.empty(), "screen_publication_missing");
    co_await Until([&] {
        auto record = receiver.listener->Find(sid);
        return record && record->frames >= 6 && record->black_left > 0 && record->white_left > 0;
    }, "remote_animated_screen_missing");
    auto record = receiver.listener->Find(sid);
    Require(record->source == Source::ScreenShareVideo, "remote_source_not_screen");
    std::cout << "[REMOTE_FRAME] source=screen frames=" << record->frames << " width=" << record->width
              << " height=" << record->height << " both_pattern_phases=true" << std::endl;
    co_return ActiveScreen{sid, std::move(record)};
}

asio::awaitable<void> Stopped(Peer& sender, Peer& receiver, const ActiveScreen& screen,
                             bool capture_ended = false, bool disconnected = false, bool restarted = false) {
    co_await Until([&] {
        auto state = sender.share->snapshot().state;
        return disconnected || state == (capture_ended ? State::Failed : State::Idle);
    }, "screen_stop_not_terminal", 35s);
    co_await Until([&] { return !receiver.RemoteHas(screen.sid); }, "remote_publication_not_removed");
    if (!disconnected) {
        Require(sender.LocalSid(Source::ScreenShareVideo).empty(), "local_screen_not_removed");
        // Restart can retire the old participant/publication; require removal
        // of any republished screen SID as well, not only the old SID.
        const auto identity = sender.room->local_participant()->identity();
        co_await Until([&] { return !receiver.RemoteHasSource(identity, Source::ScreenShareVideo); },
                      "republished_remote_screen_remains");
        if (!restarted) Require(screen.record->unpublished, "remote_unpublished_event_missing");
    }
    // Full restart / participant leave can retire the entire participant
    // instance instead of emitting a legacy per-track unsubscribe callback.
    // In those cases the remote roster removal and frame barrier are the
    // terminal observations; normal stop must also emit the track events.
    if (!restarted && !disconnected)
        Require(screen.record->unsubscribed, "remote_unsubscribed_event_missing");
    // Allow already-in-flight RTP to settle, then require no further decoded
    // frames. The local capture Stop barrier is checked independently.
    co_await Delay(300ms);
    const int captured = sender.captures->frames, received = screen.record->frames;
    co_await Delay(600ms);
    Require(sender.captures->frames == captured, "capture_continued_after_stop");
    Require(screen.record->frames == received, "remote_frames_continued_after_remove");
    Require(sender.captures->stopped > 0, "capture_stop_not_called");
}

asio::awaitable<void> Matrix(Peer& sender, Peer& receiver, bool recovery_only) {
    co_await sender.StartCamera();
    const auto original_camera = sender.LocalSid(Source::Camera);
    co_await Until([&] { auto record = receiver.listener->Find(original_camera); return record && record->frames > 2; },
                  "remote_camera_missing");
    for (int repeat = recovery_only ? 2 : 0; repeat != 2; ++repeat) {
        PatternWindow window;
        auto screen = co_await StartScreen(sender, receiver, window);
        sender.share->Stop(); sender.share->Stop();
        co_await Stopped(sender, receiver, screen);
        Require(sender.LocalSid(Source::Camera) == original_camera && receiver.RemoteHas(original_camera),
                "screen_stop_removed_camera");
        const auto camera = receiver.listener->Find(original_camera);
        const int camera_frames = camera->frames;
        co_await Until([&] { return camera->frames > camera_frames; }, "camera_stalled_after_screen_stop");
        std::cout << "[CASE] normal_stop_repeat_" << repeat << " PASS camera_preserved=true" << std::endl;
    }
    if (!recovery_only) {
        PatternWindow window;
        const int published = sender.publish_calls->load();
        sender.cancel_publish->store(true);
        sender.share->Start(window.source());
        co_await Until([&] { return sender.publish_calls->load() > published && sender.share->snapshot().state == State::Idle; },
                      "inflight_publish_cancel_not_terminal");
        co_await Delay(800ms);
        Require(sender.LocalSid(Source::ScreenShareVideo).empty(), "cancelled_screen_published");
        for (const auto& [id, participant] : receiver.room->remote_participants())
            for (const auto& [sid, publication] : participant->tracks())
                Require(!publication->track() || publication->track()->source() != Source::ScreenShareVideo,
                        "cancelled_remote_screen_remains");
        std::cout << "[CASE] cancel_during_real_publish PASS" << std::endl;
    }
    if (!recovery_only) {
        PatternWindow window;
        auto screen = co_await StartScreen(sender, receiver, window);
        window.Close();
        co_await Stopped(sender, receiver, screen, true);
        Require(sender.captures->ended > 0, "window_close_did_not_end_capture");
        std::cout << "[CASE] captured_window_closed PASS" << std::endl;
    }
    for (const auto scenario : {livekit::SimulateScenarioType::SignalReconnect, livekit::SimulateScenarioType::FullReconnect}) {
        if (recovery_only && scenario == livekit::SimulateScenarioType::SignalReconnect) continue;
        PatternWindow window;
        auto screen = co_await StartScreen(sender, receiver, window);
        const int recovered = sender.listener->reconnected, republished = sender.listener->republished;
        sender.listener->stop_on_reconnect = true;
        co_await sender.room->SimulateScenarioAsync(scenario);
        co_await Until([&] { return sender.listener->reconnected > recovered; }, "reconnect_not_completed", 40s);
        co_await Stopped(sender, receiver, screen, false, false,
            scenario == livekit::SimulateScenarioType::FullReconnect);
        const auto camera_sid = sender.LocalSid(Source::Camera);
        Require(!camera_sid.empty(), "camera_missing_after_reconnect");
        co_await Until([&] { auto record = receiver.listener->Find(camera_sid); return record && record->frames > 2; },
                      "camera_not_recovered");
        if (scenario == livekit::SimulateScenarioType::FullReconnect)
            Require(sender.listener->republished > republished, "full_restart_not_observed");
        std::cout << "[CASE] stop_during_" << (scenario == livekit::SimulateScenarioType::FullReconnect ? "full_restart" : "soft_resume")
                  << " PASS camera_recovered=true" << std::endl;
    }
    {
        PatternWindow window;
        auto screen = co_await StartScreen(receiver, sender, window);
        receiver.share->Stop();
        co_await Stopped(receiver, sender, screen);
        std::cout << "[CASE] reverse_direction PASS" << std::endl;
    }
    {
        PatternWindow window;
        auto screen = co_await StartScreen(sender, receiver, window);
        sender.share->Shutdown();
        sender.camera_running->store(false);
        co_await sender.room->DisconnectAsync();
        co_await Stopped(sender, receiver, screen, false, true);
        std::cout << "[CASE] leave_while_sharing PASS" << std::endl;
    }
}

asio::awaitable<int> Run(std::string url, std::string token, std::string peer_token, bool recovery_only) {
    auto executor = co_await asio::this_coro::executor;
    Peer first(executor), second(executor);
    int exit_code = 1;
    try {
        livekit::SignalOptions options;
        options.auto_subscribe = true;
        options.single_peer_connection = true;
        options.connect_timeout = 20s;
        options.timeouts.reconnect_total = 35s;
        co_await second.room->ConnectAsync(url, peer_token, options);
        co_await first.room->ConnectAsync(url, token, options);
        Require(first.room->local_participant()->identity() != second.room->local_participant()->identity(),
                "two_distinct_identities_required");
        Require(first.room->room_info().sid == second.room->room_info().sid, "peers_in_different_rooms");
        std::cout << "[CONNECTED] two_distinct_participants=true same_service_room=true" << std::endl;
        co_await Matrix(first, second, recovery_only);
        exit_code = 0;
    } catch (const Failure& error) {
        std::cout << "[FAILURE] code=" << error.code << std::endl;
    } catch (const livekit::OperationError& error) {
        std::cout << "[FAILURE] operation=" << int(error.operation()) << " code=" << int(error.code()) << std::endl;
    } catch (...) {
        std::cout << "[FAILURE] unexpected_exception" << std::endl;
    }
    first.Shutdown(); second.Shutdown();
    co_await Delay(250ms);
    first.room->RemoveListener(first.listener); second.room->RemoveListener(second.listener);
    std::cout << "[RESULT] SCREEN_SHARE_L3 " << (exit_code == 0 ? "PASS" : "FAIL") << std::endl;
    co_return exit_code;
}
} // namespace

int main(int argc, char** argv) {
    const bool recovery_only = argc == 2 && std::string(argv[1]) == "--recovery-and-leave";
    if (argc > 1 && !recovery_only) return 2;
    const char* url = std::getenv("LIVEKIT_URL");
    const char* token = std::getenv("LIVEKIT_TOKEN");
    const char* peer = std::getenv("LIVEKIT_PEER_TOKEN");
    if (!url || !token || !peer) { std::cout << "[RESULT] NOT_RUN missing_runtime_environment\n"; return 2; }
    asio::io_context io;
    auto result = asio::co_spawn(asio::make_strand(io), Run(url, token, peer, recovery_only), asio::use_future);
    io.run();
    try { return result.get(); }
    catch (...) { std::cout << "[RESULT] FAIL executor_exception\n"; return 1; }
}
