// Opt-in L3: two independent Room clients use a real LiveKit service. Only an
// owned, animated test window is captured; no desktop image is saved. Network
// publication, negotiation, decoding and unpublish use production code.
#include <asio.hpp>
#include "room.h"
#include "screen_share_session.h"
#include "rtc_video_source.h"
#include "webrtc_manager.h"
#include "telemetry/build_identity.h"
#include "telemetry/diagnostic_pipeline.h"
#include "telemetry/session_telemetry.h"
#include "stats.h"
#include "render/owned_i420_frame.h"
#include "share_quality_probe.h"
#include "modules/desktop_capture/desktop_capture_types.h"
#include "modules/desktop_capture/win/screen_capture_utils.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "Psapi.lib")

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
    explicit PatternWindow(bool detailed = false) {
        std::promise<HWND> ready;
        auto result = ready.get_future();
        thread_ = std::thread([ready = std::move(ready), detailed]() mutable {
            WNDCLASSW type{};
            type.lpfnWndProc = Procedure;
            type.hInstance = GetModuleHandleW(nullptr);
            type.lpszClassName = L"LiveKitScreenShareL3";
            RegisterClassW(&type);
            const bool fullscreen = std::getenv("LIVEKIT_TEST_WGC_SCREEN") != nullptr;
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
            const auto& bounds = monitor.rcMonitor;
            HWND hwnd = CreateWindowExW(fullscreen ? WS_EX_TOPMOST : 0, type.lpszClassName, L"LiveKit screen-share test pattern",
                detailed ? WS_POPUP : WS_OVERLAPPEDWINDOW,
                fullscreen ? bounds.left : 80, fullscreen ? bounds.top : 80,
                fullscreen ? bounds.right - bounds.left : 800,
                fullscreen ? bounds.bottom - bounds.top : 600,
                nullptr, nullptr, type.hInstance, nullptr);
            if (hwnd) {
                if (detailed) SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0x10000);
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                SetTimer(hwnd, 1, detailed ? 66 : 500, nullptr);
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
        if (std::getenv("LIVEKIT_TEST_WGC_SCREEN")) {
            const auto monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
            for (const auto& source : livekit::EnumerateDesktopSources()) {
                HMONITOR candidate = nullptr;
                if (source.kind == livekit::DesktopSourceKind::Screen &&
                    webrtc::GetHmonitorFromDeviceIndex(source.id, &candidate) && candidate == monitor)
                    return source;
            }
            throw Failure{"pattern_monitor_unavailable"};
        }
        return {livekit::DesktopSourceKind::Window, reinterpret_cast<intptr_t>(hwnd_), {}};
    }
private:
    static LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        if (message == WM_TIMER) {
            const auto state = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                (state & 0x10000) ? (0x10000 | ((state + 1) & 0xfff)) : !state);
            InvalidateRect(hwnd, nullptr, false);
            return 0;
        }
        if (message == WM_PAINT || message == WM_PRINTCLIENT) {
            PAINTSTRUCT paint{};
            HDC dc = message == WM_PAINT ? BeginPaint(hwnd, &paint) : reinterpret_cast<HDC>(wp);
            const auto state = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            const bool detailed = (state & 0x10000) != 0;
            const bool inverted = detailed ? ((state / 8) & 1) != 0 : state != 0;
            HDC target = dc;
            HDC buffer = detailed ? CreateCompatibleDC(dc) : nullptr;
            HBITMAP bitmap = detailed ? CreateCompatibleBitmap(dc, 800, 600) : nullptr;
            HGDIOBJ previous = bitmap ? SelectObject(buffer, bitmap) : nullptr;
            if (bitmap) dc = buffer;
            RECT rect{};
            GetClientRect(hwnd, &rect);
            if (detailed) rect = RECT{0, 0, 800, 600};
            FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(inverted ? BLACK_BRUSH : WHITE_BRUSH)));
            rect.right /= 2;
            FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(inverted ? WHITE_BRUSH : BLACK_BRUSH)));
            if (detailed) {
                RECT area{0, 0, 800, 230};
                FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
                for (int bit = 0; bit < 12; ++bit) {
                    RECT cell{32 + bit * 12, 8, 44 + bit * 12, 24};
                    FillRect(dc, &cell, static_cast<HBRUSH>(GetStockObject(
                        (state & (1 << bit)) ? WHITE_BRUSH : BLACK_BRUSH)));
                }
                SetTextColor(dc, RGB(0, 0, 0));
                SetBkColor(dc, RGB(255, 255, 255));
                auto font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
                const wchar_t text[] = L"LiveKit 0123456789 ABCDEFG abcdefg | text detail / screen share";
                for (int y = 44; y < 180; y += 18) TextOutW(dc, 16, y, text, _countof(text) - 1);
                SelectObject(dc, font);
                for (int x = 16; x < 784; x += 4) {
                    RECT line{x, 190, x + 1, 224};
                    FillRect(dc, &line, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                }
                const COLORREF colors[] = {RGB(220,30,30), RGB(30,200,30), RGB(30,30,220),
                    RGB(220,220,30), RGB(30,220,220), RGB(220,30,220)};
                for (int i = 0; i < 6; ++i) {
                    RECT bar{i * 134, 350, std::min(800, (i + 1) * 134), 440};
                    HBRUSH brush = CreateSolidBrush(colors[i]);
                    FillRect(dc, &bar, brush);
                    DeleteObject(brush);
                }
                for (int y = 450; y < 600; y += 12) for (int x = 0; x < 800; x += 12) {
                    RECT cell{x, y, std::min(800, x + 12), std::min(600, y + 12)};
                    const bool light = ((x / 12 + y / 12 + (state & 0xfff)) % 7) < 3;
                    FillRect(dc, &cell, static_cast<HBRUSH>(GetStockObject(light ? WHITE_BRUSH : BLACK_BRUSH)));
                }
                if (bitmap) {
                    RECT bounds{};
                    GetClientRect(hwnd, &bounds);
                    SetStretchBltMode(target, COLORONCOLOR);
                    StretchBlt(target, 0, 0, bounds.right, bounds.bottom, dc, 0, 0, 800, 600, SRCCOPY);
                    SelectObject(buffer, previous);
                    DeleteObject(bitmap);
                }
                if (buffer) DeleteDC(buffer);
            }
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
    std::atomic<int> frames{0}, stopped{0}, ended{0}, live{0}, next_id{0};
    std::atomic<uint32_t> backend{0};
    std::shared_ptr<ShareQualityProbe> quality;
};

const char* CapturePhaseName(livekit::DesktopCaptureProbePhase phase) {
    using Phase = livekit::DesktopCaptureProbePhase;
    switch (phase) {
    case Phase::Created: return "created";
    case Phase::Started: return "started";
    case Phase::BackendFrame: return "backend_frame";
    case Phase::Destroyed: return "destroyed";
    case Phase::Joined: return "joined";
    case Phase::SessionClosed: return "session_closed";
    case Phase::FramePoolClosed: return "frame_pool_closed";
    case Phase::D3dReleased: return "d3d_released";
    case Phase::CloseFailed: return "close_failed";
    }
    return "unknown";
}

std::uint64_t ProcessCpuTicks() {
    FILETIME created{}, exited{}, kernel{}, user{};
    Require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != 0,
            "process_cpu_sample_unavailable");
    const auto ticks = [](FILETIME value) {
        ULARGE_INTEGER count{};
        count.LowPart = value.dwLowDateTime;
        count.HighPart = value.dwHighDateTime;
        return count.QuadPart;
    };
    return ticks(kernel) + ticks(user);
}
class CountedCapture final : public livekit::IDesktopCapture {
public:
    explicit CountedCapture(std::shared_ptr<CaptureCounts> counts, bool first_frame_only = false)
        : counts_(std::move(counts)), first_frame_only_(first_frame_only) {
        ++counts_->live;
        const int id = ++counts_->next_id;
        livekit::DesktopCaptureProbeOptions probe;
        probe.allow_wgc_window = std::getenv("LIVEKIT_TEST_GDI_WINDOW") == nullptr;
        probe.simulate_dxgi_unsupported = std::getenv("LIVEKIT_TEST_WGC_SCREEN") != nullptr;
        probe.on_event = [id, counts = counts_](livekit::DesktopCaptureProbeEvent event) {
            if (event.phase == livekit::DesktopCaptureProbePhase::BackendFrame)
                counts->backend = event.capturer_id;
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
            std::cout << "[CAPTURE_PROBE] {\"capture_id\":" << id
                      << ",\"epoch_ms\":" << epoch_ms
                      << ",\"phase\":\"" << CapturePhaseName(event.phase)
                      << "\",\"thread_id\":" << event.thread_id
                      << ",\"capturer_id\":" << event.capturer_id
                      << "}" << std::endl;
        };
        capture_ = livekit::CreateDesktopCaptureForProbe(std::move(probe));
    }
    ~CountedCapture() override { --counts_->live; }
    void Start(livekit::DesktopSource source, FrameCallback frame, EndCallback end) override {
        capture_->Start(std::move(source), [counts = counts_, frame = std::move(frame),
            first_frame_only = first_frame_only_, delivered = std::make_shared<std::atomic<int>>(0)](const auto& value) {
            ++counts->frames;
            if (counts->quality) counts->quality->Capture(value);
            if (!first_frame_only || delivered->fetch_add(1) == 0) frame(value);
        }, [counts = counts_, end = std::move(end)] { ++counts->ended; end(); });
    }
    void Stop() override { capture_->Stop(); ++counts_->stopped; }
private:
    std::shared_ptr<CaptureCounts> counts_;
    std::unique_ptr<livekit::IDesktopCapture> capture_;
    bool first_frame_only_ = false;
};

struct ShareObjectLedger {
    std::vector<std::weak_ptr<livekit::LocalVideoTrack>> tracks;
    std::vector<std::weak_ptr<livekit::VideoSource>> sources;
    std::vector<std::weak_ptr<livekit::render::VideoRenderRouter>> previews;

    template <typename T>
    static std::size_t Alive(const std::vector<std::weak_ptr<T>>& values) {
        return static_cast<std::size_t>(std::count_if(values.begin(), values.end(),
            [](const auto& value) { return !value.expired(); }));
    }
};

struct ReceivedTrack {
    Source source = Source::Unknown;
    std::string rtc_track_id;
    std::atomic<int> frames{0}, valid_frames{0}, small_frames{0};
    std::atomic<int> black_left{0}, white_left{0}, width{0}, height{0};
    std::atomic<int> valid_width{0}, valid_height{0};
    std::atomic<bool> unpublished{false}, unsubscribed{false};
    livekit::Track::I420VideoFrameSubscription subscription;
};

class Listener final : public livekit::RoomListener {
public:
    std::shared_ptr<ShareQualityProbe> quality;
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
        if (auto rtc_track = track->rtc_track()) record->rtc_track_id = rtc_track->id();
        const std::weak_ptr<ReceivedTrack> weak = record;
        record->subscription = track->subscribeI420VideoFrames([weak, quality = quality](const auto& frame) {
            auto record = weak.lock();
            if (!record) return;
            if (quality && record->source == Source::ScreenShareVideo) quality->Receive(frame);
            ++record->frames;
            record->width = frame->width(); record->height = frame->height();
            if (frame->width() < 160 || frame->height() < 100) {
                ++record->small_frames;
                return;
            }
            ++record->valid_frames;
            record->valid_width = frame->width();
            record->valid_height = frame->height();
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
    void Forget(const std::string& sid) {
        std::lock_guard lock(mutex_);
        tracks_.erase(sid);
    }
private:
    asio::any_io_executor executor_;
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<ReceivedTrack>> tracks_;
};

struct Peer {
    std::shared_ptr<livekit::Room> room;
    std::string diagnostic_session_id;
    std::shared_ptr<Listener> listener;
    std::shared_ptr<livekit::ScreenShareSession> share;
    std::shared_ptr<CaptureCounts> captures = std::make_shared<CaptureCounts>();
    std::shared_ptr<ShareObjectLedger> share_objects = std::make_shared<ShareObjectLedger>();
    std::shared_ptr<std::atomic<bool>> cancel_publish = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<int>> publish_calls = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<bool>> camera_running = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<livekit::LocalVideoTrack> camera;
    explicit Peer(asio::any_io_executor executor) {
        room = livekit::Room::Create(executor);
        if (livekit::diagnostic::InstalledBusinessPipeline()) {
            livekit::diagnostic::Context context;
            context.anonymous_session_id = livekit::diagnostic::NewCorrelationId();
            diagnostic_session_id = context.anonymous_session_id.View();
            room->SetDiagnosticContext(context);
        }
        room->SetLogHandler([](const std::string& category, const std::string& tag, const std::string& message) {
            std::cout << "[NATIVE] " << category << '/' << tag;
            if (tag == "ICE_STATE" || tag == "PC_STATE" ||
                category == "ERROR" || category == "DYNACAST")
                std::cout << ' ' << message;
            if (std::getenv("LIVEKIT_TEST_MEDIA_DIAGNOSTICS") != nullptr) {
                const auto field = [&](std::string_view key) {
                    const auto begin = message.find(key);
                    if (begin == std::string::npos) return std::string{"N/A"};
                    const auto value = begin + key.size();
                    const auto end = message.find(',', value);
                    return message.substr(value, end == std::string::npos ? end : end - value);
                };
                if (tag == "PUBLISH_SENDER") {
                    std::cout << " mid=" << field("mid=")
                              << " direction=" << field("direction=")
                              << " current_direction=" << field("current_direction=")
                              << " encodings=" << field("encodings=")
                              << " active_encodings=" << field("active_encodings=");
                } else if (tag == "PUBLISH_SOURCE") {
                    std::cout << " source_delta=" << field("source_delta=")
                              << " rtc_input_delta=" << field("rtc_input_delta=");
                } else if (tag == "PUBLISH_RTP_STREAM") {
                    std::cout << " mid=" << field("mid=")
                              << " frames_encoded_delta=" << field("frames_encoded_delta=")
                              << " packets_delta=" << field("packets_delta=");
                }
            }
            std::cout << std::endl;
        });
        listener = std::make_shared<Listener>(executor);
        room->AddListener(listener);
        auto backend = livekit::ScreenShareSession::ForRoom(room);
        backend.capture = [counts = captures] {
            return std::make_unique<CountedCapture>(counts,
                std::getenv("LIVEKIT_TEST_FIRST_FRAME_ONLY") != nullptr);
        };
        auto publish = backend.publish;
        backend.publish = [publish, listener = listener, executor, cancel = cancel_publish,
                           calls = publish_calls, objects = share_objects]
                (std::shared_ptr<livekit::LocalVideoTrack> track) -> asio::awaitable<void> {
            ++*calls;
            objects->tracks.emplace_back(track);
            objects->sources.emplace_back(track->source());
            if (cancel->exchange(false)) {
                asio::post(executor, [weak = listener->share] { if (auto session = weak.lock()) session->Stop(); });
            }
            co_await publish(std::move(track));
        };
        share = std::make_shared<livekit::ScreenShareSession>(executor, std::move(backend),
            [objects = share_objects](livekit::ScreenShareSnapshot value) {
                if (value.state == State::Active && value.preview)
                    objects->previews.emplace_back(value.preview);
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
                                             std::shared_ptr<std::atomic<bool>> running,
                                             int width = 320, int height = 180) {
        auto frame = livekit::VideoFrame::create(width, height, livekit::VideoBufferType::I420);
        std::fill(frame.data(), frame.data() + frame.dataSize(), uint8_t{128});
        while (running->load()) { source->captureFrame(frame); co_await Delay(66ms); }
    }
    void Shutdown() { share->Shutdown(); camera_running->store(false); room->Disconnect(); }
};

void SampleShareObjects(const Peer& sender, int cycle, const char* phase) {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    const bool memory_ok = GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != 0;
    Require(memory_ok, "process_memory_sample_unavailable");
    DWORD handles = 0;
    Require(GetProcessHandleCount(GetCurrentProcess(), &handles) != 0,
            "process_handle_sample_unavailable");
    const HANDLE threads = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    Require(threads != INVALID_HANDLE_VALUE, "process_thread_sample_unavailable");
    THREADENTRY32 thread{};
    thread.dwSize = sizeof(thread);
    DWORD thread_count = 0;
    std::vector<DWORD> thread_ids;
    if (Thread32First(threads, &thread)) {
        do {
            if (thread.th32OwnerProcessID == GetCurrentProcessId()) {
                ++thread_count;
                thread_ids.push_back(thread.th32ThreadID);
            }
            thread.dwSize = sizeof(thread);
        } while (Thread32Next(threads, &thread));
    }
    CloseHandle(threads);
    std::sort(thread_ids.begin(), thread_ids.end());
    const auto native = sender.room->GetPublisherMediaObjectCounts();
    const auto& objects = *sender.share_objects;
    std::cout << "[LIFECYCLE] {\"cycle\":" << cycle
              << ",\"phase\":\"" << phase << "\""
              << ",\"private_bytes\":" << memory.PrivateUsage
              << ",\"handles\":" << handles
              << ",\"threads\":" << thread_count
              << ",\"gdi_objects\":" << GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)
              << ",\"user_objects\":" << GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS)
              << ",\"capture_live\":" << sender.captures->live.load()
              << ",\"track_live\":" << ShareObjectLedger::Alive(objects.tracks)
              << ",\"source_live\":" << ShareObjectLedger::Alive(objects.sources)
              << ",\"preview_live\":" << ShareObjectLedger::Alive(objects.previews)
              << ",\"rtc_video_source_live\":" << livekit::RtcVideoSource::LiveInstanceCount()
              << ",\"publisher_transceivers\":" << native.transceivers
              << ",\"publisher_senders_with_track\":" << native.senders_with_track
              << ",\"publisher_senders_without_track\":" << native.senders_without_track
              << ",\"thread_ids\":[";
    for (std::size_t i = 0; i < thread_ids.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << thread_ids[i];
    }
    std::cout << "]}" << std::endl;
}

struct ActiveScreen { std::string sid; std::shared_ptr<ReceivedTrack> record; };
asio::awaitable<ActiveScreen> StartScreen(Peer& sender, Peer& receiver, PatternWindow& window) {
    sender.share->Start(window.source());
    co_await Until([&] { return sender.share->snapshot().state == State::Active; }, "screen_publish_not_active");
    const auto sid = sender.LocalSid(Source::ScreenShareVideo);
    Require(!sid.empty(), "screen_publication_missing");
    try {
        co_await Until([&] {
            auto record = receiver.listener->Find(sid);
            return record && record->frames >= 6 && record->black_left > 0 && record->white_left > 0;
        }, "remote_animated_screen_missing");
    } catch (const Failure&) {
        const auto record = receiver.listener->Find(sid);
        bool remote_published = false;
        bool remote_media_bound = false;
        int remote_participants = 0;
        bool current_sender_present = false;
        bool sender_screen_published = !sender.LocalSid(Source::ScreenShareVideo).empty();
        const auto sender_local = sender.room->local_participant();
        for (const auto& [participant_sid, participant] : receiver.room->remote_participants()) {
            if (!participant) continue;
            ++remote_participants;
            if (sender_local && participant->sid() == sender_local->sid()) {
                current_sender_present = true;
            }
            if (const auto publication = participant->get_publication(sid)) {
                remote_published = true;
                remote_media_bound = publication->track() && publication->track()->rtc_track();
            }
        }
        std::cout << "[SCREEN_DELIVERY_STATE] sender_connected="
                  << (sender.room->connection_state() == livekit::ConnectionState::Connected)
                  << " receiver_connected="
                  << (receiver.room->connection_state() == livekit::ConnectionState::Connected)
                  << " sender_screen_published=" << sender_screen_published
                  << " receiver_participants=" << remote_participants
                  << " current_sender_present=" << current_sender_present
                  << " remote_published=" << remote_published
                  << " remote_media_bound=" << remote_media_bound
                  << " subscribed=" << static_cast<bool>(record)
                  << " frames=" << (record ? record->frames.load() : 0)
                  << " black_left=" << (record ? record->black_left.load() : 0)
                  << " white_left=" << (record ? record->white_left.load() : 0)
                  << std::endl;
        throw;
    }
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
    try {
        co_await Until([&] { return !receiver.RemoteHas(screen.sid); }, "remote_publication_not_removed");
    } catch (const Failure&) {
        int remote_participants = 0;
        int remote_screen_publications = 0;
        int sender_identity_instances = 0;
        bool old_screen_on_current_sender = false;
        const auto sender_local = sender.room->local_participant();
        for (const auto& [sid, participant] : receiver.room->remote_participants()) {
            if (!participant) continue;
            ++remote_participants;
            if (sender_local && participant->identity() == sender_local->identity()) {
                ++sender_identity_instances;
            }
            if (sender_local && participant->sid() == sender_local->sid() &&
                participant->get_publication(screen.sid)) {
                old_screen_on_current_sender = true;
            }
            for (const auto& [track_sid, publication] : participant->tracks()) {
                if (publication && publication->track() &&
                    publication->track()->source() == Source::ScreenShareVideo) {
                    ++remote_screen_publications;
                }
            }
        }
        std::cout << "[CLEANUP_STATE] restarted=" << restarted
                  << " sender_connected=" << (sender.room->connection_state() == livekit::ConnectionState::Connected)
                  << " sender_screen_publication=" << !sender.LocalSid(Source::ScreenShareVideo).empty()
                  << " receiver_participants=" << remote_participants
                  << " sender_identity_instances=" << sender_identity_instances
                  << " receiver_screen_publications=" << remote_screen_publications
                  << " receiver_old_sid=" << receiver.RemoteHas(screen.sid)
                  << " old_screen_on_current_sender=" << old_screen_on_current_sender
                  << " republished_events=" << sender.listener->republished.load()
                  << " remote_unpublished=" << screen.record->unpublished.load()
                  << " remote_unsubscribed=" << screen.record->unsubscribed.load()
                  << std::endl;
        throw;
    }
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

asio::awaitable<void> NetworkFaultMatrix(
        Peer& sender, Peer& receiver,
        const std::shared_ptr<livekit::telemetry::SessionTelemetry>& telemetry) {
    co_await sender.StartCamera();
    const auto camera_sid = sender.LocalSid(Source::Camera);
    co_await Until([&] {
        const auto record = receiver.listener->Find(camera_sid);
        return record && record->frames > 4;
    }, "network_probe_remote_camera_missing");
    const int reconnecting = receiver.listener->reconnecting;
    const int reconnected = receiver.listener->reconnected;
    std::cout << "[FAULT_READY] signaling_tcp" << std::endl;
    co_await Until([&] { return receiver.listener->reconnecting > reconnecting; },
        "network_fault_not_observed", 25s);
    co_await Until([&] { return receiver.listener->reconnected > reconnected; },
        "network_reconnect_not_completed", 60s);
    const auto after_reconnect = receiver.listener->Find(camera_sid);
    const int baseline = after_reconnect ? after_reconnect->frames.load() : 0;
    co_await Until([&] {
        const auto current = receiver.listener->Find(camera_sid);
        return current && (current == after_reconnect
            ? current->frames > baseline + 4 : current->frames > 4);
    }, "network_media_not_recovered", 20s);
    co_await Until([&] {
        const auto snapshot = telemetry->SnapshotOnStrand();
        return snapshot && snapshot->reconnect_video_expected >= 1 &&
            snapshot->reconnect_video_recovered >= 1 &&
            snapshot->reconnect_video_availability ==
                livekit::telemetry::Availability::Valid;
    }, "network_recovery_telemetry_missing", 15s);
    std::cout << "[CASE] actual_signaling_network_fault PASS camera_recovered=true"
              << std::endl;
}

asio::awaitable<void> LifecycleMatrix(Peer& sender, Peer& receiver) {
    co_await sender.StartCamera();
    const auto camera_sid = sender.LocalSid(Source::Camera);
    co_await Until([&] {
        auto record = receiver.listener->Find(camera_sid);
        return record && record->frames > 2;
    }, "remote_camera_missing");
    PatternWindow window;
    const auto baseline_native = sender.room->GetPublisherMediaObjectCounts();
    const auto baseline_sources = livekit::RtcVideoSource::LiveInstanceCount();
    SampleShareObjects(sender, 0, "baseline");
    for (int cycle = 1; cycle <= 12; ++cycle) {
        {
            auto screen = co_await StartScreen(sender, receiver, window);
            co_await Delay(1500ms);
            SampleShareObjects(sender, cycle, "active");
            sender.share->Stop();
            co_await Stopped(sender, receiver, screen);
            receiver.listener->Forget(screen.sid);
        }
        co_await Delay(1500ms);
        co_await Until([&] {
            const auto native = sender.room->GetPublisherMediaObjectCounts();
            return native.transceivers == baseline_native.transceivers &&
                native.senders_with_track == baseline_native.senders_with_track &&
                livekit::RtcVideoSource::LiveInstanceCount() == baseline_sources;
        }, "retired_screen_resources_not_reclaimed", 5s);
        Require(sender.captures->live == 0 &&
                ShareObjectLedger::Alive(sender.share_objects->tracks) == 0 &&
                ShareObjectLedger::Alive(sender.share_objects->sources) == 0 &&
                ShareObjectLedger::Alive(sender.share_objects->previews) == 0,
                "screen_objects_retained_after_stop");
        SampleShareObjects(sender, cycle, "stopped");
    }
    co_await Delay(10s);
    SampleShareObjects(sender, 12, "settled");
}

asio::awaitable<void> PublisherLifecycleMatrix(Peer& sender, bool direct_track) {
    co_await sender.StartCamera();
    std::unique_ptr<PatternWindow> window;
    if (!direct_track) window = std::make_unique<PatternWindow>();
    livekit::VideoPublishOptions share_options;
    if (const char* codec = std::getenv("LIVEKIT_TEST_SHARE_CODEC")) {
        share_options.video_codec = codec;
    }
    int cycles = 12;
    if (const char* requested = std::getenv("LIVEKIT_TEST_SHARE_CYCLES")) {
        const int value = std::atoi(requested);
        if (value >= 1 && value <= 12) cycles = value;
    }
    SampleShareObjects(sender, 0, "baseline");
    for (int cycle = 1; cycle <= cycles; ++cycle) {
        std::shared_ptr<livekit::LocalVideoTrack> direct;
        std::shared_ptr<std::atomic<bool>> feeding;
        std::string direct_sid;
        if (direct_track) {
            auto source = std::make_shared<livekit::VideoSource>(800, 600);
            direct = livekit::LocalVideoTrack::createLocalVideoTrack(
                "direct_screen_" + std::to_string(cycle), source,
                Source::ScreenShareVideo, share_options);
            feeding = std::make_shared<std::atomic<bool>>(true);
            asio::co_spawn(sender.room->executor(),
                Peer::CameraFrames(source, feeding, 800, 600), asio::detached);
            direct_sid = (co_await sender.room->local_participant()->PublishTrackAsync(direct))->sid();
        } else {
            sender.share->Start(window->source(), share_options);
            co_await Until([&] { return sender.share->snapshot().state == State::Active; },
                           "screen_publish_not_active");
        }
        co_await Delay(1500ms);
        SampleShareObjects(sender, cycle, "active");
        if (direct_track) {
            feeding->store(false);
            co_await sender.room->local_participant()->UnpublishTrackAsync(direct_sid);
            direct.reset();
        } else {
            sender.share->Stop();
            co_await Until([&] { return sender.share->snapshot().state == State::Idle; },
                           "screen_stop_not_terminal", 35s);
        }
        Require(sender.LocalSid(Source::ScreenShareVideo).empty(), "local_screen_not_removed");
        co_await Delay(1500ms);
        SampleShareObjects(sender, cycle, "stopped");
    }
    co_await Delay(10s);
    SampleShareObjects(sender, cycles, "settled");
}

template <typename T>
nlohmann::json Measured(bool available, const T& value) {
    return available ? nlohmann::json(value) : nlohmann::json(nullptr);
}

asio::awaitable<void> SampleShareRtc(Peer& peer, const char* role, const char* phase,
                                     const std::string& screen_sid,
                                     const std::string& rtc_track_id) {
    const auto stats = co_await peer.room->GetStats();
    nlohmann::json sample = {
        {"role", role}, {"phase", phase}, {"screen_sid", screen_sid},
        {"rtc_track_id", rtc_track_id}, {"epoch_ms", stats.timestamp_ms},
        {"successful_pc", stats.successful_peer_connection_count},
        {"timed_out_pc", stats.timed_out_peer_connection_count},
        {"rejected_pc", stats.rejected_peer_connection_count},
        {"outbound", nlohmann::json::array()},
        {"inbound", nlohmann::json::array()},
        {"remote_inbound", nlohmann::json::array()},
        {"senders", nlohmann::json::array()},
        {"candidate_pairs", nlohmann::json::array()},
        {"codecs", nlohmann::json::array()}
    };
    if (std::string_view(role) == "sender") {
        auto local = peer.room->local_participant();
        auto publication = local ? local->get_publication(screen_sid) : nullptr;
        auto video = publication ? std::dynamic_pointer_cast<livekit::LocalVideoTrack>(
            publication->track()) : nullptr;
        if (video) {
            const auto frames = video->frame_diagnostics();
            sample["source_frames"] = Measured(frames.source_available, frames.source_frames);
            sample["rtc_input_frames"] = Measured(frames.rtc_available, frames.rtc_input_frames);
            sample["rtc_output_frames"] = Measured(frames.rtc_available, frames.rtc_output_frames);
            sample["rtc_dropped_frames"] = Measured(frames.rtc_available, frames.rtc_dropped_frames);
            const auto options = video->publish_options();
            sample["publish_codec"] = options.video_codec;
            sample["publish_layers"] = nlohmann::json::array();
            for (const auto& layer : options.layers) {
                sample["publish_layers"].push_back({
                    {"rid", layer.rid}, {"width", layer.width}, {"height", layer.height},
                    {"max_fps", layer.max_fps}, {"max_bitrate_bps", layer.max_bitrate_bps}
                });
            }
        }
    }
    for (const auto& report : stats.reports) {
        for (const auto& sender : report.senders) {
            if (sender.kind != "video") continue;
            sample["senders"].push_back({
                {"track_id", sender.track_id},
                {"mid", Measured(sender.mid_available, sender.mid)},
                {"active_encodings", sender.active_encoding_count}
            });
        }
        for (const auto& stream : report.outbound_rtp) {
            if (stream.kind != "video") continue;
            sample["outbound"].push_back({
                {"id", stream.id}, {"ssrc", stream.ssrc},
                {"mid", Measured(stream.mid_available, stream.mid)},
                {"rid", Measured(stream.rid_available, stream.rid)},
                {"codec_id", Measured(stream.codec_id_available, stream.codec_id)},
                {"bytes_sent", Measured(stream.bytes_sent_available, stream.bytes_sent)},
                {"packets_sent", Measured(stream.packets_sent_available, stream.packets_sent)},
                {"frames_encoded", Measured(stream.frames_encoded_available, stream.frames_encoded)},
                {"frames_sent", Measured(stream.frames_sent_available, stream.frames_sent)},
                {"fps", Measured(stream.frames_per_second_available, stream.frames_per_second)},
                {"retransmitted_packets", Measured(stream.retransmitted_packets_sent_available,
                                                    stream.retransmitted_packets_sent)},
                {"quality_limitation_reason", Measured(stream.quality_limitation_reason_available,
                                                        stream.quality_limitation_reason)},
                {"quality_limitation_durations", Measured(stream.quality_limitation_durations_available,
                                                           stream.quality_limitation_durations)}
            });
        }
        for (const auto& stream : report.inbound_rtp) {
            if (stream.kind != "video") continue;
            sample["inbound"].push_back({
                {"id", stream.id}, {"ssrc", stream.ssrc},
                {"track_identifier", Measured(stream.track_identifier_available,
                                              stream.track_identifier)},
                {"mid", Measured(stream.mid_available, stream.mid)},
                {"codec_id", Measured(stream.codec_id_available, stream.codec_id)},
                {"bytes_received", Measured(stream.bytes_received_available, stream.bytes_received)},
                {"packets_received", Measured(stream.packets_received_available, stream.packets_received)},
                {"packets_lost", Measured(stream.packets_lost_available, stream.packets_lost)},
                {"frames_received", Measured(stream.frames_received_available, stream.frames_received)},
                {"frames_decoded", Measured(stream.frames_decoded_available, stream.frames_decoded)},
                {"frames_dropped", Measured(stream.frames_dropped_available, stream.frames_dropped)},
                {"fps", Measured(stream.frames_per_second_available, stream.frames_per_second)},
                {"nack_count", Measured(stream.nack_count_available, stream.nack_count)},
                {"pli_count", Measured(stream.pli_count_available, stream.pli_count)}
            });
        }
        for (const auto& stream : report.remote_inbound_rtp) {
            sample["remote_inbound"].push_back({
                {"ssrc", stream.ssrc}, {"local_id", Measured(stream.local_id_available, stream.local_id)},
                {"fraction_lost", Measured(stream.fraction_lost_available, stream.fraction_lost)},
                {"round_trip_time", Measured(stream.round_trip_time_available, stream.round_trip_time)}
            });
        }
        for (const auto& pair : report.candidate_pairs) {
            if (!pair.current_pair) continue;
            sample["candidate_pairs"].push_back({
                {"id", pair.id}, {"rtt", Measured(pair.current_round_trip_time_available,
                                                   pair.current_round_trip_time)},
                {"available_outgoing_bitrate", Measured(pair.available_outgoing_bitrate_available,
                                                          pair.available_outgoing_bitrate)},
                {"bytes_sent", Measured(pair.bytes_sent_available, pair.bytes_sent)},
                {"bytes_received", Measured(pair.bytes_received_available, pair.bytes_received)}
            });
        }
        for (const auto& codec : report.codecs) {
            if (codec.mime_type.find("video/") == 0)
                sample["codecs"].push_back({{"id", codec.id}, {"mime_type", codec.mime_type}});
        }
    }
    std::cout << "[SHARE_RTC] " << sample.dump() << std::endl;
}

asio::awaitable<void> PerformanceMatrix(Peer& sender, Peer& receiver) {
    const bool quality_only = std::getenv("LIVEKIT_TEST_SHARE_QUALITY") != nullptr;
    if (quality_only) {
        const bool fullscreen = std::getenv("LIVEKIT_TEST_WGC_SCREEN") != nullptr;
        sender.captures->quality = std::make_shared<ShareQualityProbe>(
            fullscreen ? GetSystemMetrics(SM_CXSCREEN) : 800,
            fullscreen ? GetSystemMetrics(SM_CYSCREEN) : 600);
        receiver.listener->quality = sender.captures->quality;
    }
    co_await sender.StartCamera();
    const auto camera_sid = sender.LocalSid(Source::Camera);
    co_await Until([&] {
        auto record = receiver.listener->Find(camera_sid);
        return record && record->frames > 2;
    }, "remote_camera_missing");

    const auto baseline_cpu_start = ProcessCpuTicks();
    const auto baseline_wall_start = std::chrono::steady_clock::now();
    co_await Delay(10s);
    const auto baseline_wall_end = std::chrono::steady_clock::now();
    const auto baseline_cpu_end = ProcessCpuTicks();

    PatternWindow window(true);
    auto screen = co_await StartScreen(sender, receiver, window);
    if (std::getenv("LIVEKIT_TEST_WGC_SCREEN")) {
        Require(sender.captures->backend == webrtc::DesktopCapturerId::kWgcCapturerWin,
                "fullscreen_probe_not_wgc");
        std::cout << "[SCREEN_SOURCE] kind=monitor backend=wgc width=" << GetSystemMetrics(SM_CXSCREEN)
                  << " height=" << GetSystemMetrics(SM_CYSCREEN) << std::endl;
    }
    // Measure steady publication after congestion-control/layer startup. The
    // earlier 2-second probe remains a separate startup-quality failure record.
    co_await Delay(15s);
    const auto publication = sender.room->local_participant()->get_publication(screen.sid);
    Require(publication && publication->track() && publication->track()->rtc_track(),
            "screen_sender_rtc_track_missing");
    const auto sender_track_id = publication->track()->rtc_track()->id();
    co_await SampleShareRtc(sender, "sender", "start", screen.sid, sender_track_id);
    co_await SampleShareRtc(receiver, "receiver", "start", screen.sid,
                            screen.record->rtc_track_id);
    const int capture_start = sender.captures->frames.load();
    const int remote_start = screen.record->frames.load();
    const int valid_start = screen.record->valid_frames.load();
    const int small_start = screen.record->small_frames.load();
    const auto active_cpu_start = ProcessCpuTicks();
    const auto active_wall_start = std::chrono::steady_clock::now();
    const char* sample_seconds_env = std::getenv("LIVEKIT_TEST_SHARE_SAMPLE_SECONDS");
    const int sample_seconds = sample_seconds_env ? std::atoi(sample_seconds_env) : 30;
    Require(sample_seconds >= 30 && sample_seconds <= 180, "invalid_share_sample_duration");
    for (int interval = 1; interval <= sample_seconds; ++interval) {
        co_await Delay(1s);
        if (quality_only) std::cout << "[SHARE_QUALITY_SAMPLE] "
            << sender.captures->quality->Sample().dump() << std::endl;
        if (interval % 10 != 0) continue;
        const char* phase = interval == 10 ? "t10" : interval == 20 ? "t20" : "end";
        co_await SampleShareRtc(sender, "sender", phase, screen.sid, sender_track_id);
        co_await SampleShareRtc(receiver, "receiver", phase, screen.sid,
                                screen.record->rtc_track_id);
    }
    const auto active_wall_end = std::chrono::steady_clock::now();
    const auto active_cpu_end = ProcessCpuTicks();
    const int captured = sender.captures->frames.load() - capture_start;
    const int received = screen.record->frames.load() - remote_start;
    const int valid_received = screen.record->valid_frames.load() - valid_start;
    const int small_received = screen.record->small_frames.load() - small_start;
    const int last_width = screen.record->width.load();
    const int last_height = screen.record->height.load();
    const int valid_width = screen.record->valid_width.load();
    const int valid_height = screen.record->valid_height.load();
    Require(captured > 0 && received > 0, "screen_perf_no_frame_progress");
    sender.share->Stop();
    co_await Stopped(sender, receiver, screen);

    if (quality_only) {
        const auto result = sender.captures->quality->Summary();
        std::cout << "[SHARE_QUALITY] " << result.dump() << std::endl;
        Require(result.at("status") == "PASS", "screen_quality_threshold_failed");
        co_return;
    }

    const double baseline_seconds = std::chrono::duration<double>(
        baseline_wall_end - baseline_wall_start).count();
    const double active_seconds = std::chrono::duration<double>(
        active_wall_end - active_wall_start).count();
    const double baseline_cpu = static_cast<double>(
        baseline_cpu_end - baseline_cpu_start) / 1e7 / baseline_seconds * 100.0;
    const double active_cpu = static_cast<double>(
        active_cpu_end - active_cpu_start) / 1e7 / active_seconds * 100.0;
    std::cout << "[SHARE_PERF] {\"warmup_seconds\":15,\"baseline_seconds\":" << baseline_seconds
              << ",\"active_seconds\":" << active_seconds
              << ",\"capture_frames\":" << captured
              << ",\"remote_frames\":" << received
              << ",\"remote_valid_frames\":" << valid_received
              << ",\"remote_small_frames\":" << small_received
              << ",\"capture_fps\":" << captured / active_seconds
              << ",\"remote_fps\":" << received / active_seconds
              << ",\"remote_valid_fps\":" << valid_received / active_seconds
              << ",\"baseline_cpu_core_percent\":" << baseline_cpu
              << ",\"active_cpu_core_percent\":" << active_cpu
              << ",\"cpu_increment_core_percent\":" << active_cpu - baseline_cpu
              << ",\"logical_processors\":" << std::thread::hardware_concurrency()
              << ",\"remote_width\":" << last_width
              << ",\"remote_height\":" << last_height
              << ",\"remote_valid_width\":" << valid_width
              << ",\"remote_valid_height\":" << valid_height
              << "}" << std::endl;
}

asio::awaitable<int> Run(std::string url, std::string peer_url,
                        std::string token, std::string peer_token,
                        bool recovery_only, bool lifecycle_only, bool publisher_only,
                        bool direct_track, bool performance_only, bool network_only,
                        asio::strand<asio::io_context::executor_type> strand) {
    auto executor = co_await asio::this_coro::executor;
    Peer first(executor);
    auto second = publisher_only ? nullptr : std::make_unique<Peer>(executor);
    std::shared_ptr<livekit::telemetry::SessionTelemetry> receiver_telemetry;
    if (network_only && second) {
        receiver_telemetry = std::make_shared<livekit::telemetry::SessionTelemetry>(
            strand, 1, livekit::telemetry::SessionTelemetry::kDefaultQueueCapacity,
            livekit::telemetry::SessionTelemetry::Clock::now(), nullptr,
            second->diagnostic_session_id);
        second->room->SetSessionTelemetry(receiver_telemetry);
    }
    int exit_code = 1;
    try {
        livekit::SignalOptions options;
        options.auto_subscribe = true;
        options.single_peer_connection = true;
        const char* allow_insecure = std::getenv("LIVEKIT_TEST_ALLOW_INSECURE");
        options.allow_insecure_transport = allow_insecure && std::string(allow_insecure) == "1";
        options.connect_timeout = 20s;
        options.timeouts.reconnect_total = 35s;
        if (second) co_await second->room->ConnectAsync(peer_url, peer_token, options);
        co_await first.room->ConnectAsync(url, token, options);
        if (second) {
            Require(first.room->local_participant()->identity() != second->room->local_participant()->identity(),
                    "two_distinct_identities_required");
            Require(first.room->room_info().sid == second->room->room_info().sid, "peers_in_different_rooms");
            std::cout << "[CONNECTED] two_distinct_participants=true same_service_room=true" << std::endl;
        }
        if (network_only) co_await NetworkFaultMatrix(first, *second, receiver_telemetry);
        else if (performance_only) co_await PerformanceMatrix(first, *second);
        else if (publisher_only) co_await PublisherLifecycleMatrix(first, direct_track);
        else if (lifecycle_only) co_await LifecycleMatrix(first, *second);
        else co_await Matrix(first, *second, recovery_only);
        exit_code = 0;
    } catch (const Failure& error) {
        std::cout << "[FAILURE] code=" << error.code << std::endl;
    } catch (const livekit::OperationError& error) {
        std::cout << "[FAILURE] operation=" << int(error.operation()) << " code=" << int(error.code()) << std::endl;
    } catch (...) {
        std::cout << "[FAILURE] unexpected_exception" << std::endl;
    }
    first.Shutdown();
    if (second) second->Shutdown();
    co_await Delay(250ms);
    if (receiver_telemetry) receiver_telemetry->StopOnStrand();
    first.room->RemoveListener(first.listener);
    if (second) second->room->RemoveListener(second->listener);
    std::cout << "[RESULT] SCREEN_SHARE_L3 " << (exit_code == 0 ? "PASS" : "FAIL") << std::endl;
    co_return exit_code;
}
} // namespace

int main(int argc, char** argv) {
    if (std::getenv("LIVEKIT_TEST_WGC_SCREEN"))
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool recovery_only = argc == 2 && std::string(argv[1]) == "--recovery-and-leave";
    const bool lifecycle_only = argc == 2 && std::string(argv[1]) == "--lifecycle-probe";
    const bool publisher_only = argc == 2 && std::string(argv[1]) == "--publisher-lifecycle-probe";
    const bool direct_track = argc == 2 && std::string(argv[1]) == "--direct-track-lifecycle-probe";
    const bool performance_only = argc == 2 && std::string(argv[1]) == "--performance-probe";
    const bool network_only = argc == 2 && std::string(argv[1]) == "--network-fault-probe";
    if (argc > 1 && !recovery_only && !lifecycle_only && !publisher_only &&
        !direct_track && !performance_only && !network_only) return 2;
    const char* url = std::getenv("LIVEKIT_URL");
    const char* token = std::getenv("LIVEKIT_TOKEN");
    const char* peer = std::getenv("LIVEKIT_PEER_TOKEN");
    const char* peer_url = std::getenv("LIVEKIT_PEER_URL");
    if (!url || !token || (!peer && !publisher_only && !direct_track) ||
        (network_only && (!peer_url || !std::getenv("LIVEKIT_TEST_DIAG_ROOT")))) {
        std::cout << "[RESULT] NOT_RUN missing_runtime_environment\n";
        return 2;
    }
    std::shared_ptr<livekit::diagnostic::DiagnosticPipeline> diagnostics;
    if (const char* root = std::getenv("LIVEKIT_TEST_DIAG_ROOT")) {
        diagnostics = std::make_shared<livekit::diagnostic::DiagnosticPipeline>();
        livekit::diagnostic::InstallBusinessPipeline(diagnostics);
        diagnostics->TryEmit(livekit::diagnostic::Event::Started(
            livekit::telemetry::CurrentExecutableBuildId(),
            livekit::telemetry::CurrentExecutablePdbIdentity()));
        if (!diagnostics->StartWriter(std::filesystem::path(root))) {
            livekit::diagnostic::InstallBusinessPipeline({});
            std::cout << "[RESULT] FAIL diagnostic_writer_start\n";
            return 3;
        }
    }
    asio::io_context io;
    auto strand = asio::make_strand(io);
    auto result = asio::co_spawn(strand,
        Run(url, peer_url ? peer_url : url, token, peer ? peer : "",
            recovery_only, lifecycle_only, publisher_only || direct_track,
            direct_track, performance_only, network_only, strand), asio::use_future);
    io.run();
    int exit_code = 1;
    try { exit_code = result.get(); }
    catch (...) { std::cout << "[RESULT] FAIL executor_exception\n"; }
    if (diagnostics) {
        const auto drained = diagnostics->Close();
        const auto status = diagnostics->GetStatus();
        livekit::diagnostic::InstallBusinessPipeline({});
        std::cout << "[DIAGNOSTIC] accepted=" << status.accepted
                  << " written=" << status.written
                  << " dropped_ordinary=" << status.dropped_ordinary
                  << " dropped_critical=" << status.dropped_critical
                  << " sink_failures=" << status.sink_failures
                  << " drain=" << static_cast<int>(drained) << std::endl;
        if (drained != livekit::diagnostic::DrainResult::Completed ||
            status.written == 0 || status.sink_failures != 0 ||
            status.dropped_critical != 0) {
            return 4;
        }
    }
    return exit_code;
}
