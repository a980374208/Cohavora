#include "tests/support/test_check.h"
#include "screen_share_session.h"
#include "room.h"
#include "rtc_video_source.h"
#include "livekit_rtc.pb.h"
#include <atomic>
#include <iostream>
#include <future>
#include <thread>

using namespace std::chrono_literals;

namespace livekit {
class RoomUnpublishTestAccess {
public:
    static void InstallPublisher(Room& room, std::shared_ptr<LocalParticipant> local,
                                 webrtc::scoped_refptr<webrtc::PeerConnectionInterface> publisher) {
        room.SetLocalParticipantForTesting(std::move(local));
        room.publisher_pc_ = std::move(publisher);
        room.connection_state_ = ConnectionState::Connected;
    }
    static void Install(Room& room, std::shared_ptr<LocalParticipant> local,
                        std::function<asio::awaitable<void>()> negotiate) {
        room.SetLocalParticipantForTesting(local);
        room.connection_state_ = ConnectionState::Connected;
        room.local_unpublish_test_hooks_ = std::make_shared<Room::LocalUnpublishTestHooks>();
        room.local_unpublish_test_hooks_->remove_sender = [](std::shared_ptr<Track>, uint64_t) -> asio::awaitable<void> {
            co_return;
        };
        room.local_unpublish_test_hooks_->negotiate = [negotiate](auto, auto) { return negotiate(); };
        const std::weak_ptr<Room> weak = room.shared_from_this();
        local->SetAsyncUnpublishTrackHandler([weak](std::string sid) {
            return weak.lock()->UnpublishLocalTrackAsync(std::move(sid));
        });
    }
};
}

namespace {
using livekit::ScreenShareState;
using livekit::ScreenShareError;

struct CaptureState {
    livekit::IDesktopCapture::FrameCallback frame;
    livekit::IDesktopCapture::EndCallback ended;
    int starts = 0, stops = 0;
    bool fail = false;
    void Emit() { frame(livekit::VideoFrame::create(1920, 1080, livekit::VideoBufferType::I420)); }
};
class FakeCapture final : public livekit::IDesktopCapture {
public:
    explicit FakeCapture(std::shared_ptr<CaptureState> state) : state_(std::move(state)) {}
    void Start(livekit::DesktopSource, FrameCallback frame, EndCallback ended) override {
        ++state_->starts;
        state_->frame = std::move(frame);
        state_->ended = std::move(ended);
        if (state_->fail) throw std::runtime_error("capture denied");
    }
    void Stop() override { ++state_->stops; }
private:
    std::shared_ptr<CaptureState> state_;
};

struct Fixture {
    asio::io_context io;
    asio::strand<asio::io_context::executor_type> strand{io.get_executor()};
    std::shared_ptr<livekit::Room> room = livekit::Room::Create(io.get_executor());
    std::shared_ptr<livekit::LocalParticipant> local = std::make_shared<livekit::LocalParticipant>(
        "PA_SHARE", "share", [](const auto&) {});
    std::shared_ptr<CaptureState> capture = std::make_shared<CaptureState>();
    std::shared_ptr<livekit::ScreenShareSession> share;
    std::shared_ptr<livekit::VideoSource> source;
    std::weak_ptr<livekit::LocalVideoTrack> track;
    asio::steady_timer publish_gate{strand};
    asio::steady_timer stop_gate{strand};
    bool hold_publish = false, fail_publish = false, hold_stop = false, fail_stop = false;
    int publishes = 0, unpublishes = 0, frames = 0;
    livekit::proto::SignalRequest wire;
    std::vector<livekit::ScreenShareSnapshot> states;

    explicit Fixture(
            std::optional<livekit::ScreenBinding> resolvedBinding = std::nullopt,
            std::shared_ptr<std::atomic<bool>> bindingValid = {}) {
        publish_gate.expires_at(asio::steady_timer::time_point::max());
        stop_gate.expires_at(asio::steady_timer::time_point::max());
        livekit::RoomUnpublishTestAccess::Install(*room, local, [this] { return NegotiateStop(); });
        local->SetAsyncPublishTrackHandler([this](std::shared_ptr<livekit::Track> value,
                                                   const livekit::proto::SignalRequest& request) {
            return Publish(std::move(value), request);
        });
        auto camera = std::make_shared<livekit::Track>("TR_CAMERA", "camera", livekit::TrackKind::Video,
                                                      livekit::TrackSource::Camera);
        local->add_publication(std::make_shared<livekit::TrackPublication>(camera, "TR_CAMERA", "camera"));
        auto backend = livekit::ScreenShareSession::ForRoom(room);
        backend.capture = [this] { return std::make_unique<FakeCapture>(capture); };
        backend.resolve_screen_binding = [binding = std::move(resolvedBinding)](
                const livekit::DesktopSource &source,
                std::uint64_t sourceEpoch,
                std::string shareSessionId) mutable {
            if (!binding) return std::optional<livekit::ScreenBinding>{};
            auto result = *binding;
            result.source_id = source.id;
            result.source_epoch = sourceEpoch;
            result.share_session_id = std::move(shareSessionId);
            return std::optional<livekit::ScreenBinding>{std::move(result)};
        };
        backend.validate_screen_binding = [bindingValid](const livekit::ScreenBinding &) {
            return !bindingValid || bindingValid->load(std::memory_order_acquire);
        };
        backend.first_frame_timeout = 80ms;
        backend.geometry_check_interval = 1ms;
        share = std::make_shared<livekit::ScreenShareSession>(strand, std::move(backend),
            [this](auto state) { states.push_back(state); });
    }
    ~Fixture() {
        Do([&] { share->Shutdown(); publish_gate.cancel(); stop_gate.cancel(); });
        share.reset();
        io.restart(); io.poll();
        room.reset();
    }
    asio::awaitable<std::shared_ptr<livekit::TrackPublication>> Publish(
        std::shared_ptr<livekit::Track> value, livekit::proto::SignalRequest request) {
        ++publishes;
        wire = std::move(request);
        auto video = std::dynamic_pointer_cast<livekit::LocalVideoTrack>(value);
        TEST_CHECK(video && video->Track::source() == livekit::TrackSource::ScreenShareVideo);
        source = video->source();
        track = video;
        value->addVideoSink([this](const auto&, const auto&) { ++frames; });
        if (hold_publish) {
            std::error_code error;
            co_await publish_gate.async_wait(asio::redirect_error(asio::use_awaitable, error));
        }
        if (fail_publish) throw std::runtime_error("server denied");
        value->set_sid("TR_SCREEN");
        auto publication = std::make_shared<livekit::TrackPublication>(value, "TR_SCREEN", "screen");
        local->add_publication(publication);
        co_return publication;
    }
    asio::awaitable<void> NegotiateStop() {
        ++unpublishes;
        if (hold_stop) {
            std::error_code error;
            co_await stop_gate.async_wait(asio::redirect_error(asio::use_awaitable, error));
        }
        if (fail_stop) throw std::runtime_error("answer timeout");
    }
    void Do(std::function<void()> fn) { asio::post(strand, std::move(fn)); io.restart(); io.poll(); }
    template<class Predicate> void Until(Predicate ready) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!ready() && std::chrono::steady_clock::now() < deadline) {
            io.restart(); io.run_for(5ms);
        }
        TEST_CHECK(ready());
    }
    void Start(bool frame = true) {
        Do([&] { share->Start({}); });
        TEST_CHECK(capture->starts == 1);
        if (frame) capture->Emit();
    }
    ScreenShareState State() const { return states.empty() ? ScreenShareState::Idle : states.back().state; }
};

void NormalAndRepeat() {
    Fixture f;
    f.Start();
    f.Until([&] { return f.State() == ScreenShareState::Active; });
    TEST_CHECK(f.states.back().source_kind == livekit::DesktopSourceKind::Screen);
    TEST_CHECK(!f.states.back().annotation_binding);
    TEST_CHECK(f.wire.add_track().source() == livekit::proto::SCREEN_SHARE);
    TEST_CHECK(f.wire.add_track().width() == 1920 && f.wire.add_track().height() == 1080);
    TEST_CHECK(f.wire.add_track().layers_size() == 2);
    f.Do([&] { f.share->Start({}); });
    TEST_CHECK(f.publishes == 1);
    f.capture->Emit();
    TEST_CHECK(f.frames == 1);
    const auto preview = f.states.back().preview;
    TEST_CHECK(preview && preview->active());
    auto previewFrame = preview->TakeLatest("screen", preview->generation());
    TEST_CHECK(previewFrame && previewFrame->width() == 1920 && previewFrame->height() == 1080);
    auto old_callback = f.capture->frame;
    f.Do([&] { f.share->Stop(); f.share->Stop(); });
    f.Until([&] { return f.State() == ScreenShareState::Idle; });
    TEST_CHECK(f.capture->stops == 1 && f.unpublishes == 1);
    TEST_CHECK(!preview->active() && !preview->TakeLatest("screen", preview->generation()));
    TEST_CHECK(!f.states.back().preview);
    TEST_CHECK(f.local->get_publication("TR_SCREEN") == nullptr);
    TEST_CHECK(f.local->get_publication("TR_CAMERA") != nullptr);
    TEST_CHECK(f.track.expired()); // source does not retain the local track
    old_callback(livekit::VideoFrame::create(8, 8, livekit::VideoBufferType::I420));
    TEST_CHECK(f.frames == 1);
    f.capture = std::make_shared<CaptureState>();
    f.Start();
    f.Until([&] { return f.State() == ScreenShareState::Active; });
    old_callback(livekit::VideoFrame::create(8, 8, livekit::VideoBufferType::I420));
    TEST_CHECK(f.source->width() == 1920 && f.publishes == 2);
}

void ScreenBindingLifecycle() {
    livekit::ScreenBinding binding;
    binding.display_name = "DISPLAY1";
    binding.device_key = L"DISPLAY1";
    binding.physical_width = 1920;
    binding.physical_height = 1080;
    binding.canonical_width = 1920;
    binding.canonical_height = 1080;
    auto valid = std::make_shared<std::atomic<bool>>(true);
    Fixture f(binding, valid);
    const livekit::DesktopSource screen{livekit::DesktopSourceKind::Screen, 77, "bound screen"};
    f.Do([&] { f.share->Start(screen); });
    f.capture->Emit();
    f.Until([&] { return f.State() == ScreenShareState::Active; });
    const auto active = f.states.back();
    TEST_CHECK(active.annotation_binding.has_value());
    TEST_CHECK(active.source_kind == livekit::DesktopSourceKind::Screen);
    TEST_CHECK(active.annotation_binding->source_id == 77);
    TEST_CHECK(active.annotation_binding->source_epoch == 1);
    TEST_CHECK(active.annotation_binding->share_session_id.rfind("share-", 0) == 0);
    TEST_CHECK(active.annotation_binding->share_session_id.size() == 38);
    valid->store(false, std::memory_order_release);
    f.Until([&] { return f.State() == ScreenShareState::Failed; });
    TEST_CHECK(f.states.back().error == ScreenShareError::Capture);
    TEST_CHECK(!f.states.back().annotation_binding);
    TEST_CHECK(f.capture->stops == 1 && f.unpublishes == 1);

    Fixture window(binding, std::make_shared<std::atomic<bool>>(true));
    window.Do([&] { window.share->Start(
        {livekit::DesktopSourceKind::Window, 88, "window"}); });
    window.capture->Emit();
    window.Until([&] { return window.State() == ScreenShareState::Active; });
    TEST_CHECK(window.states.back().source_kind == livekit::DesktopSourceKind::Window);
    TEST_CHECK(!window.states.back().annotation_binding);
}

void CancelPublishAndLeave() {
    for (bool leave : {false, true}) {
        Fixture f;
        f.hold_publish = true;
        f.Start();
        f.Until([&] { return f.publishes == 1; });
        TEST_CHECK(f.State() == ScreenShareState::Starting);
        f.Do([&] { if (leave) f.share->Shutdown(); else f.share->Stop(); });
        TEST_CHECK(f.capture->stops == 1);
        const auto count = f.states.size();
        f.capture->Emit();
        TEST_CHECK(f.frames == 0);
        f.Do([&] { f.publish_gate.cancel(); });
        if (leave) TEST_CHECK(f.states.size() == count);
        else {
            f.Until([&] { return f.State() == ScreenShareState::Idle; });
            TEST_CHECK(f.unpublishes == 1 && !f.local->get_publication("TR_SCREEN"));
        }
        for (const auto& state : f.states) TEST_CHECK(state.state != ScreenShareState::Active);
    }
}

void FailuresAndEnded() {
    for (int mode = 0; mode != 5; ++mode) {
        Fixture f;
        f.capture->fail = mode == 0;
        f.fail_publish = mode == 2;
        f.Start(mode >= 2);
        if (mode < 3) {
            f.Until([&] { return f.State() == ScreenShareState::Failed; });
            TEST_CHECK(f.capture->stops == 1);
            TEST_CHECK(f.publishes == (mode == 2 ? 1 : 0));
        } else {
            f.Until([&] { return f.State() == ScreenShareState::Active; });
            if (mode == 3) {
                f.capture->ended();
                f.Until([&] { return f.State() == ScreenShareState::Failed; });
                TEST_CHECK(f.states.back().error == ScreenShareError::Capture);
                TEST_CHECK(!f.local->get_publication("TR_SCREEN"));
            } else {
                f.fail_stop = true;
                f.Do([&] { f.share->Stop(); });
                f.Until([&] { return f.State() == ScreenShareState::StopFailed; });
                TEST_CHECK(f.local->get_publication("TR_SCREEN") != nullptr);
                TEST_CHECK(f.capture->stops == 1);
                f.Do([&] { f.share->Start({}); });
                TEST_CHECK(f.publishes == 1);
            }
        }
    }
}

void ReconnectAndConfirmation() {
    Fixture f;
    f.hold_stop = true;
    f.Start();
    f.Until([&] { return f.State() == ScreenShareState::Active; });
    f.Do([&] { f.share->SetTransportReady(false); f.share->Stop(); });
    TEST_CHECK(f.capture->stops == 1 && f.unpublishes == 0);
    // Simulate the new SID assigned to the same Track by full reconnect.
    auto current = f.track.lock();
    f.local->remove_publication("TR_SCREEN");
    current->set_sid("TR_RECOVERED");
    f.local->add_publication(std::make_shared<livekit::TrackPublication>(current, "TR_RECOVERED", "screen"));
    current.reset();
    f.Do([&] { f.share->SetTransportReady(true); });
    f.Until([&] { return f.unpublishes == 1; });
    TEST_CHECK(f.State() == ScreenShareState::Stopping);
    TEST_CHECK(f.local->get_publication("TR_RECOVERED") != nullptr);
    f.Do([&] { f.stop_gate.cancel(); });
    f.Until([&] { return f.State() == ScreenShareState::Idle; });
    TEST_CHECK(f.local->get_publication("TR_RECOVERED") == nullptr);
}

struct Sink : webrtc::VideoSinkInterface<webrtc::VideoFrame> {
    int width = 0, height = 0, frames = 0;
    void OnFrame(const webrtc::VideoFrame& frame) override {
        width = frame.width(); height = frame.height(); ++frames;
    }
};
void FrameBridgeLifetime() {
    auto source = std::make_shared<livekit::VideoSource>(1920, 1080);
    auto rtc = livekit::RtcVideoSource::Create(source, true);
    TEST_CHECK(rtc->is_screencast());
    Sink sink;
    static_cast<webrtc::VideoTrackSourceInterface*>(rtc.get())->AddOrUpdateSink(&sink, webrtc::VideoSinkWants{});
    const auto frame = livekit::VideoFrame::create(1920, 1080, livekit::VideoBufferType::I420);
    source->captureFrame(frame);
    TEST_CHECK(sink.width == 1920 && sink.height == 1080 && sink.frames == 1);
    static_cast<webrtc::VideoTrackSourceInterface*>(rtc.get())->RemoveSink(&sink);
    rtc = nullptr;
    source->captureFrame(frame); // retained source must not call a destroyed RTC adapter
    auto camera = livekit::RtcVideoSource::Create(source);
    TEST_CHECK(!camera->is_screencast());
    static_cast<webrtc::VideoTrackSourceInterface*>(camera.get())->AddOrUpdateSink(&sink, webrtc::VideoSinkWants{});
    source->captureFrame(frame);
    TEST_CHECK(sink.width == 1920 && sink.height == 1080 && sink.frames == 2);
    static_cast<webrtc::VideoTrackSourceInterface*>(camera.get())->RemoveSink(&sink);
}

void InFlightFrameTeardown() {
    struct BlockingSink : webrtc::VideoSinkInterface<webrtc::VideoFrame> {
        std::promise<void> entered, release;
        std::shared_future<void> gate = release.get_future().share();
        std::atomic<int> frames{0};
        void OnFrame(const webrtc::VideoFrame&) override {
            ++frames;
            entered.set_value();
            gate.wait();
        }
    } sink;
    auto entered = sink.entered.get_future();
    auto source = std::make_shared<livekit::VideoSource>(8, 8);
    auto rtc = livekit::RtcVideoSource::Create(source, true);
    static_cast<webrtc::VideoTrackSourceInterface*>(rtc.get())->AddOrUpdateSink(&sink, webrtc::VideoSinkWants{});
    auto frame = livekit::VideoFrame::create(8, 8, livekit::VideoBufferType::I420);
    std::thread producer([&] { source->captureFrame(frame); });
    TEST_CHECK(entered.wait_for(2s) == std::future_status::ready);
    std::promise<void> destroying, destroyed;
    auto starting = destroying.get_future();
    auto finished = destroyed.get_future();
    std::thread destroyer([owned = std::move(rtc), &destroying, &destroyed]() mutable {
        destroying.set_value();
        owned = nullptr;
        destroyed.set_value();
    });
    TEST_CHECK(starting.wait_for(2s) == std::future_status::ready);
    TEST_CHECK(finished.wait_for(20ms) == std::future_status::timeout);
    sink.release.set_value();
    producer.join();
    destroyer.join();
    source->captureFrame(frame);
    TEST_CHECK(sink.frames.load() == 1);
}

void SubscriptionCancellation() {
    auto source = std::make_shared<livekit::VideoSource>(8, 8);
    std::promise<void> entered, release;
    auto entered_future = entered.get_future();
    auto gate = release.get_future().share();
    auto first = source->subscribe([&](const auto&, const auto&) { entered.set_value(); gate.wait(); });
    int delivered = 0;
    auto second = source->subscribe([&](const auto&, const auto&) { ++delivered; });
    std::thread producer([&] { source->captureFrame(livekit::VideoFrame::create(8, 8, livekit::VideoBufferType::I420)); });
    TEST_CHECK(entered_future.wait_for(2s) == std::future_status::ready);
    second.reset(); // copied pending delivery must not keep its RAII token alive
    release.set_value();
    producer.join();
    TEST_CHECK(delivered == 0);
}

void RemoteSourceProjection() {
    asio::io_context io;
    auto room = livekit::Room::Create(io.get_executor());
    livekit::proto::ParticipantUpdate update;
    auto* participant = update.add_participants();
    participant->set_sid("PA_SOURCE"); participant->set_identity("source-peer");
    participant->set_state(livekit::proto::ParticipantInfo::ACTIVE);
    auto* video = participant->add_tracks();
    video->set_sid("TR_SOURCE"); video->set_type(livekit::proto::VIDEO);
    video->set_source(livekit::proto::SCREEN_SHARE);
    room->UpdateParticipantsForTesting(update);
    const auto remote = room->remote_participants().at("PA_SOURCE");
    const auto track = remote->get_publication("TR_SOURCE")->track();
    TEST_CHECK(track->source() == livekit::TrackSource::ScreenShareVideo);
    video->set_source(livekit::proto::CAMERA);
    room->UpdateParticipantsForTesting(update);
    TEST_CHECK(remote->get_publication("TR_SOURCE")->track() == track);
    TEST_CHECK(track->source() == livekit::TrackSource::Camera);
}

void DynacastPublicationIsolation() {
    struct Observer final : webrtc::PeerConnectionObserver {
        void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
        void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
        void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
        void OnIceCandidate(const webrtc::IceCandidateInterface*) override {}
    } observer;
    TEST_CHECK(livekit::WebRTCManager::Instance().Initialize());
    auto factory = livekit::WebRTCManager::Instance().factory();
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    auto result = factory->CreatePeerConnectionOrError(config, webrtc::PeerConnectionDependencies(&observer));
    TEST_CHECK(result.ok());
    auto publisher = result.MoveValue();
    auto local = std::make_shared<livekit::LocalParticipant>("PA_QUALITY", "quality-peer", [](const auto&) {});
    const auto add = [&](const char* name, const char* rid, const char* sid, livekit::TrackSource source) {
        auto track = livekit::LocalVideoTrack::createLocalVideoTrack(name,
            std::make_shared<livekit::VideoSource>(320, 180), source);
        local->add_publication(std::make_shared<livekit::TrackPublication>(track, sid, name));
        webrtc::RtpTransceiverInit init;
        init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
        webrtc::RtpEncodingParameters encoding;
        encoding.rid = rid; encoding.active = true;
        init.send_encodings.push_back(encoding);
        auto added = publisher->AddTransceiver(track->rtc_track(), init);
        TEST_CHECK(added.ok());
        return added.MoveValue()->sender();
    };
    auto camera = add("quality-camera", "q", "TR_QUALITY_CAMERA", livekit::TrackSource::Camera);
    auto screen = add("quality-screen", "h", "TR_QUALITY_SCREEN", livekit::TrackSource::ScreenShareVideo);
    asio::io_context io;
    auto room = livekit::Room::Create(io.get_executor());
    livekit::RoomUnpublishTestAccess::InstallPublisher(*room, local, publisher);
    const auto update = [&](const char* sid, bool low, bool medium, bool high) {
        livekit::proto::SignalResponse message;
        auto* quality = message.mutable_subscribed_quality_update();
        quality->set_track_sid(sid);
        for (const auto& [level, enabled] : std::vector<std::pair<livekit::proto::VideoQuality, bool>>{
                {livekit::proto::LOW, low}, {livekit::proto::MEDIUM, medium}, {livekit::proto::HIGH, high}}) {
            auto* layer = quality->add_subscribed_qualities();
            layer->set_quality(level); layer->set_enabled(enabled);
        }
        room->HandleSignalMessageForTesting(message);
    };
    update("TR_QUALITY_CAMERA", true, false, false);
    TEST_CHECK(camera->GetParameters().encodings.front().active);
    TEST_CHECK(screen->GetParameters().encodings.front().active);
    update("TR_QUALITY_SCREEN", false, false, false);
    TEST_CHECK(camera->GetParameters().encodings.front().active);
    TEST_CHECK(!screen->GetParameters().encodings.front().active);
    update("TR_RETIRED", false, false, false);
    TEST_CHECK(camera->GetParameters().encodings.front().active);
    room->Disconnect();
    io.run();
}
}

int main() {
    NormalAndRepeat();
    ScreenBindingLifecycle();
    CancelPublishAndLeave();
    FailuresAndEnded();
    ReconnectAndConfirmation();
    FrameBridgeLifetime();
    InFlightFrameTeardown();
    SubscriptionCancellation();
    RemoteSourceProjection();
    DynacastPublicationIsolation();
    std::cout << "SCREEN_SHARE_SESSION: lifecycle, source mapping, resolution and in-flight teardown PASS\n";
}
