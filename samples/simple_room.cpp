#include <iostream>
#include <string>
#include <string_view>
#include <syncstream>
#include <vector>
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <thread>
#include <atomic>
#include <array>
#include <cmath>
#include <iomanip>
#include <numbers>
#include <asio.hpp>
#include "room.h"
#include "participant.h"
#include "chat_message.h"
#include "audio_source.h"
#include "video_source.h"
#include "local_audio_track.h"
#include "local_video_track.h"
#include "audio_frame.h"
#include "video_frame.h"
#include "key_provider.h"
#include "log_redaction.h"
#include "frame_cryptor.h"
#include "render/owned_i420_frame.h"
#include "rtc/webrtc_manager.h"
#include "telemetry/diagnostic_pipeline.h"
#include "telemetry/telemetry.h"

// 产生 48kHz 单声道模拟音频白噪声/正弦采样帧
void RunAudioCaptureLoop(std::shared_ptr<livekit::AudioSource> source, std::atomic<bool>& running) {
    auto frame = livekit::AudioFrame::create(48000, 1, 480); // 10ms @ 48kHz (480 采样点)
    int step = 0;
    const char* tone = std::getenv("COHAVORA_SAMPLE_AUDIO_TONE_HZ");
    const bool tone880 = tone && std::string_view(tone) == "880";
    std::uint64_t tone_sample = 0;
    while (running.load()) {
        for (size_t i = 0; i < frame.data().size(); ++i) {
            frame.data()[i] = tone880
                ? static_cast<int16_t>(9000.0 * std::sin(2.0 * std::numbers::pi * 880.0 *
                    static_cast<double>(tone_sample++ % 48000) / 48000.0))
                : static_cast<int16_t>((step + i) % 2000 - 1000);
        }
        step += 10;
        source->captureFrame(frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// 产生全屏 RGB 色块轮播 1280x720 @ 30FPS 视频帧
void RunVideoCaptureLoop(std::shared_ptr<livekit::VideoSource> source, std::atomic<bool>& running) {
    const int width = 1280;
    const int height = 720;
    auto frame = livekit::VideoFrame::create(width, height, livekit::VideoBufferType::RGBA);
    
    struct ColorRGBA { uint8_t r, g, b, a; const char* name; };
    const ColorRGBA color_palette[] = {
        {220, 38, 38, 255, "RED"},
        {22, 163, 74, 255, "GREEN"},
        {37, 99, 235, 255, "BLUE"},
        {147, 51, 234, 255, "PURPLE"},
        {15, 23, 42, 255, "DARK"}
    };
    const size_t num_colors = sizeof(color_palette) / sizeof(color_palette[0]);

    int frame_count = 0;
    size_t color_idx = 0;

    std::cout << "[Video Engine] Live video frame generator thread started (1280x720 @ 30FPS)." << std::endl;

    while (running.load()) {
        if (frame_count % 30 == 0) {
            color_idx = (frame_count / 30) % num_colors;
        }

        const auto& cur_color = color_palette[color_idx];
        uint8_t* pdata = frame.data();
        if (pdata) {
            uint32_t pixel_val = (static_cast<uint32_t>(cur_color.a) << 24) |
                                 (static_cast<uint32_t>(cur_color.b) << 16) |
                                 (static_cast<uint32_t>(cur_color.g) << 8)  |
                                 static_cast<uint32_t>(cur_color.r);
            uint32_t* p32 = reinterpret_cast<uint32_t*>(pdata);
            const size_t total_pixels = static_cast<size_t>(width * height);
            for (size_t i = 0; i < total_pixels; ++i) {
                p32[i] = pixel_val;
            }
        }

        livekit::VideoCaptureOptions vopts;
        vopts.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        vopts.rotation = livekit::VideoRotation::VIDEO_ROTATION_0;

        source->captureFrame(frame, vopts);
        frame_count++;
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
}

struct PlayoutProbe {
    bool ensured = false;
    bool playing = false;
};

PlayoutProbe ProbePlayout() {
    auto& manager = livekit::WebRTCManager::Instance();
    const bool ensured = manager.EnsurePlayout();
    const auto adm = manager.adm();
    auto* worker = manager.worker_thread();
    const bool playing = adm && worker && worker->BlockingCall(
        [device = adm.get()] { return device->Playing(); });
    return {ensured, playing};
}

// Private synthetic-audio runs can bind an owned non-default null sink before
// any PeerConnection/remote PCM exists. Never select a fallback endpoint or
// print the supplied description; the fixture retains that sink until exit.
bool BindPrivatePlayoutRoute() {
    const char* description = std::getenv("COHAVORA_SAMPLE_PLAYOUT_DESCRIPTION");
    if (!description) return true;
#if defined(WEBRTC_LINUX)
    auto& manager = livekit::WebRTCManager::Instance();
    if (!*description || !manager.Initialize()) {
        std::osyncstream(std::cout) << "[AUDIO ROUTE DIAG] event=failed reason=initialize\n";
        return false;
    }
    std::string selected;
    unsigned matches = 0;
    for (const auto& device : manager.ListPlayoutDevices()) {
        if (!device.id.empty() && device.name == description) {
            selected = device.id;
            ++matches;
        }
    }
    if (matches != 1 || !manager.SetPlayoutDeviceById(selected)) {
        std::osyncstream(std::cout) << "[AUDIO ROUTE DIAG] event=failed reason=select\n";
        manager.Deinitialize();
        return false;
    }
    std::osyncstream(std::cout) << "[AUDIO ROUTE DIAG] event=bound\n";
    return true;
#else
    std::osyncstream(std::cout) << "[AUDIO ROUTE DIAG] event=failed reason=unsupported\n";
    return false;
#endif
}

// SimpleRoom 核心事件监听器
class SimpleRoomListener : public livekit::RoomListener {
public:
    ~SimpleRoomListener() override { RetireAllAudio(); }
    void OnConnected() override {
        std::cout << "\n==================================================" << std::endl;
        std::cout << "[SUCCESS] SimpleRoom::OnConnected - Successfully connected to LiveKit Room!" << std::endl;
        std::cout << "==================================================" << std::endl;
    }

    void OnDisconnected(const std::string& reason) override {
        RetireAllAudio();
        std::cout << "\n[INFO] SimpleRoom::OnDisconnected - Reason: "
                  << (reason.empty() ? "Normal Disconnect"
                                     : livekit::secure_log::OpaqueSummary("room_disconnect"))
                  << std::endl;
    }

    void OnReconnecting() override {
        reception_state_->reconnect_seen.store(true, std::memory_order_release);
        std::cout << "[WARN] SimpleRoom::OnReconnecting - Network unstable, reconnecting..." << std::endl;
    }

    void OnReconnected() override {
        const auto epoch = reception_state_->reconnect_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
        const auto playout = ProbePlayout();
        std::osyncstream(std::cout) << "[MEDIA DIAG] kind=reconnected reconnect_seen=1 epoch="
            << epoch << " audio_subscriptions="
            << reception_state_->audio_subscriptions.load(std::memory_order_acquire)
            << " playout=" << (playout.ensured ? 1 : 0)
            << " playing=" << (playout.playing ? 1 : 0) << '\n';
        std::cout << "[INFO] SimpleRoom::OnReconnected - Connection restored!" << std::endl;
    }

    void OnParticipantConnected(std::shared_ptr<livekit::RemoteParticipant> participant) override {
        if (participant) {
            std::cout << "[EVENT] Remote Participant Joined: identity='" << participant->identity()
                      << "', sid=" << participant->sid() << std::endl;
        }
    }

    void OnParticipantDisconnected(std::shared_ptr<livekit::RemoteParticipant> participant) override {
        if (participant) {
            {
                std::lock_guard lock(audio_subscriptions_mutex_);
                std::erase_if(audio_subscriptions_, [&](const auto& entry) {
                    if (entry.second.participant_sid != participant->sid()) return false;
                    RetireAudio(entry.second.reception);
                    return true;
                });
            }
            {
                std::lock_guard lock(video_subscriptions_mutex_);
                std::erase_if(video_subscriptions_, [&](const auto& entry) {
                    return entry.second.participant_sid == participant->sid();
                });
            }
            std::cout << "[EVENT] Remote Participant Left: identity='" << participant->identity()
                      << "', sid=" << participant->sid() << std::endl;
        }
    }

    void OnTrackPublished(std::shared_ptr<livekit::RemoteParticipant> participant,
                          std::shared_ptr<livekit::TrackPublication> publication) override {
        if (participant && publication) {
            std::string kind_str = (publication->track() && publication->track()->kind() == livekit::TrackKind::Audio) ? "AUDIO" : "VIDEO";
            std::cout << "[EVENT] Remote User [" << participant->identity()
                      << "] published " << kind_str << " track: name='" << publication->name()
                      << "', sid=" << publication->sid() << std::endl;
        }
    }

    void OnTrackSubscribed(std::shared_ptr<livekit::Track> track,
                           std::shared_ptr<livekit::TrackPublication> publication,
                           std::shared_ptr<livekit::RemoteParticipant> participant) override {
        if (participant && publication) {
            std::string kind_str = (track && track->kind() == livekit::TrackKind::Audio) ? "AUDIO" : "VIDEO";
            std::cout << "[EVENT] Subscribed to " << kind_str << " track from ["
                      << participant->identity() << "]: name='" << publication->name()
                      << "', sid=" << publication->sid() << std::endl;

            if (track && track->kind() == livekit::TrackKind::Audio) {
                auto state = reception_state_;
                bool sink_registered = false;
                std::shared_ptr<AudioReception> audio;
                {
                    std::lock_guard lock(audio_subscriptions_mutex_);
                    std::erase_if(audio_sink_tracks_, [](const auto& item) { return item.first.expired(); });
                    if (auto old = audio_subscriptions_.find(publication->sid()); old != audio_subscriptions_.end()) {
                        // Duplicate subscription events do not install a second sink.
                        if (old->second.track.lock() != track) {
                            RetireAudio(old->second.reception);
                            audio_subscriptions_.erase(old);
                        } else {
                            audio = old->second.reception;
                        }
                    }
                    if (!audio && audio_subscriptions_.size() < 256) {
                        auto& registrations = audio_sink_tracks_[track];
                        // addAudioSink has no removal token. Weak per-generation
                        // closures and a fixed per-track cap bound repeat events.
                        if (registrations < 64) {
                            ++registrations;
                            audio = std::make_shared<AudioReception>();
                            audio->source = track->source();
                            audio->generation = state->audio_sequence.fetch_add(1) + 1;
                            audio_subscriptions_.emplace(publication->sid(), AudioSubscription{
                                participant->sid(), track, audio});
                            sink_registered = true;
                            AudioDiagnostic("subscribed", *audio);
                        }
                    }
                }
                if (sink_registered) {
                    track->addAudioSink([weak = std::weak_ptr<AudioReception>(audio), state](const livekit::AudioFrame& frame) {
                        auto audio = weak.lock();
                        if (!audio || frame.totalSamples() == 0) return;
                        std::lock_guard lock(audio->mutex);
                        if (!audio->accepting || !audio->Accumulate(frame)) return;
                        if (audio->frames == 1) {
                            std::cout << "[RECV AUDIO SINK] First frame received: sample_rate="
                                      << frame.sampleRate() << ", channels=" << frame.numChannels() << std::endl;
                            livekit::Telemetry::Instance().OnFirstRemoteFrameReceived("audio");
                            AudioDiagnostic("first_frame", *audio);
                        }
                        if (audio->frames % 100 == 0) {
                            std::cout << "[RECV AUDIO SINK] Frame checkpoint" << std::endl;
                        }
                        const auto epoch = state->reconnect_epoch.load(std::memory_order_acquire);
                        if (epoch && state->audio_frame_epoch.exchange(epoch) != epoch) {
                            std::cout << "[RECV AUDIO SINK] First frame after reconnect" << std::endl;
                        }
                    });
                } else if (!audio) {
                    std::osyncstream(std::cout) << "[AUDIO DIAG LIMIT] kind=subscription_capacity\n";
                }
                const auto subscriptions = state->audio_subscriptions.fetch_add(
                    1, std::memory_order_acq_rel) + 1;
                const bool reconnect_seen = state->reconnect_seen.load(std::memory_order_acquire);
                const auto epoch = state->reconnect_epoch.load(std::memory_order_acquire);
                const auto playout = ProbePlayout();
                std::osyncstream(std::cout) << "[MEDIA DIAG] kind=audio_subscribed reconnect_seen="
                    << (reconnect_seen ? 1 : 0) << " epoch=" << epoch
                    << " audio_subscriptions=" << subscriptions
                    << " playout=" << (playout.ensured ? 1 : 0)
                    << " playing=" << (playout.playing ? 1 : 0)
                    << " sink_registered=" << (sink_registered ? 1 : 0) << '\n';
            } else if (track && track->kind() == livekit::TrackKind::Video) {
                auto first_frame_flag = std::make_shared<std::atomic<bool>>(false);
                auto frame_count = std::make_shared<std::atomic<uint64_t>>(0);
                auto state = reception_state_;
                auto screen = std::make_shared<ScreenReception>();
                screen->enabled = track->source() == livekit::TrackSource::ScreenShareVideo;
                if (screen->enabled) {
                    screen->sequence = state->screen_sequence.fetch_add(1) + 1;
                    ScreenDiagnostic("subscribed", *screen);
                }
                auto subscription = track->subscribeI420VideoFrames(
                    [user = participant->identity(), first_frame_flag, frame_count, state, screen](livekit::render::OwnedI420Frame::Ptr frame) {
                        if (!frame) return;
                        {
                            std::lock_guard lock(screen->mutex);
                            if (!screen->accepting) return;
                            if (screen->enabled) {
                                ++screen->frames;
                                screen->width = frame->width();
                                screen->height = frame->height();
                                if (screen->frames == 1) ScreenDiagnostic("first_frame", *screen);
                                else if (screen->frames % 30 == 0) ScreenDiagnostic("checkpoint", *screen);
                            }
                        }
                        const auto count = frame_count->fetch_add(1, std::memory_order_relaxed) + 1;
                        if (!first_frame_flag->exchange(true)) {
                            std::cout << "[RECV VIDEO SINK] First frame received from [" << user
                                      << "]: resolution=" << frame->width() << "x" << frame->height() << std::endl;
                            livekit::Telemetry::Instance().OnFirstRemoteFrameReceived("video");
                        }
                        if (count % 30 == 0) {
                            std::cout << "[RECV VIDEO SINK] Frame checkpoint" << std::endl;
                        }
                        const auto epoch = state->reconnect_epoch.load(std::memory_order_acquire);
                        if (epoch && state->video_frame_epoch.exchange(epoch) != epoch) {
                            std::cout << "[RECV VIDEO SINK] First frame after reconnect" << std::endl;
                        }
                    });
                std::lock_guard lock(video_subscriptions_mutex_);
                video_subscriptions_.insert_or_assign(publication->sid(), VideoSubscription{
                    participant->sid(), std::move(subscription), std::move(screen)});
            }
        }
    }

    void OnTrackUnsubscribed(std::shared_ptr<livekit::Track> track,
                             std::shared_ptr<livekit::TrackPublication> publication,
                             std::shared_ptr<livekit::RemoteParticipant> participant) override {
        if (participant && publication) {
            {
                std::lock_guard lock(audio_subscriptions_mutex_);
                if (const auto entry = audio_subscriptions_.find(publication->sid());
                    entry != audio_subscriptions_.end() && (!track || entry->second.track.lock() == track)) {
                    RetireAudio(entry->second.reception);
                    audio_subscriptions_.erase(entry);
                }
            }
            if (track && track->kind() == livekit::TrackKind::Video) {
                std::lock_guard lock(video_subscriptions_mutex_);
                if (const auto entry = video_subscriptions_.find(publication->sid());
                    entry != video_subscriptions_.end()) {
                    const auto& screen = entry->second.screen;
                    std::lock_guard delivery_lock(screen->mutex);
                    screen->accepting = false;
                    if (screen->enabled) ScreenDiagnostic("unsubscribed", *screen);
                }
                video_subscriptions_.erase(publication->sid());
            }
            std::cout << "[EVENT] Unsubscribed from track '" << publication->name()
                      << "' of user [" << participant->identity() << "]" << std::endl;
        }
    }

    void OnTrackMuted(std::shared_ptr<livekit::Participant> participant,
                      std::shared_ptr<livekit::TrackPublication> publication,
                      bool muted) override {
        if (participant && publication) {
            std::cout << "[EVENT] Track Mute Status Changed: user='" << participant->identity()
                      << "', track='" << publication->name()
                      << "', muted=" << (muted ? "true" : "false") << std::endl;
        }
    }

    std::vector<std::string> last_speakers_;
    std::chrono::steady_clock::time_point last_speaker_log_time_;

    void OnActiveSpeakersChanged(const std::vector<std::shared_ptr<livekit::Participant>>& speakers) override {
        std::vector<std::string> current_speakers;
        for (const auto& spk : speakers) {
            if (spk) current_speakers.push_back(spk->identity());
        }
        std::sort(current_speakers.begin(), current_speakers.end());

        auto now = std::chrono::steady_clock::now();
        // Debounce: suppress repetitive identical speaker updates within 2 seconds
        if (current_speakers == last_speakers_) {
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_speaker_log_time_).count();
            if (elapsed_ms < 2000) {
                return;
            }
        }

        last_speakers_ = current_speakers;
        last_speaker_log_time_ = now;

        std::cout << "[EVENT] Active Speakers Updated (" << speakers.size() << "): ";
        for (const auto& spk : speakers) {
            if (spk) std::cout << spk->identity() << " ";
        }
        std::cout << std::endl;
    }

    void OnDataReceived(const std::vector<uint8_t>& payload,
                        std::shared_ptr<livekit::RemoteParticipant> participant,
                        const std::string& topic) override {
        std::string sender = participant ? participant->identity() : "Server/System";
        std::string content(payload.begin(), payload.end());
        std::cout << "[DATA RECV] Topic='" << topic << "', Sender='" << sender
                  << "', Bytes=" << payload.size() << ", Content: " << content << std::endl;
    }

    void OnChatMessage(const livekit::ChatMessage& message,
                       std::shared_ptr<livekit::Participant> participant) override {
        std::string sender = participant ? participant->identity() : message.sender_identity;
        std::cout << "[CHAT RECV] User [" << sender << "]: " << message.message << std::endl;
    }

    void OnE2eeStateChanged(const std::string& participant_identity,
                            const std::string& track_sid,
                            livekit::EncryptionState state) override {
        const char* state_str = "UNKNOWN";
        switch (state) {
            case livekit::EncryptionState::NEW: state_str = "NEW"; break;
            case livekit::EncryptionState::OK: state_str = "OK (ENCRYPTED)"; break;
            case livekit::EncryptionState::ENCRYPTION_FAILED: state_str = "ENCRYPTION_FAILED"; break;
            case livekit::EncryptionState::DECRYPTION_FAILED: state_str = "DECRYPTION_FAILED"; break;
            case livekit::EncryptionState::MISSING_KEY: state_str = "MISSING_KEY"; break;
            case livekit::EncryptionState::INTERNAL_ERROR: state_str = "INTERNAL_ERROR"; break;
        }
        std::cout << "[E2EE STATUS] Identity='" << participant_identity
                  << "', TrackSid='" << track_sid
                  << "', State=" << state_str << std::endl;
    }

private:
    struct ReceptionState {
        std::atomic<bool> reconnect_seen{false};
        std::atomic<uint64_t> reconnect_epoch{0};
        std::atomic<uint64_t> audio_subscriptions{0};
        std::atomic<uint64_t> audio_frame_epoch{0};
        std::atomic<uint64_t> video_frame_epoch{0};
        std::atomic<uint64_t> screen_sequence{0};
        std::atomic<uint64_t> audio_sequence{0};
    };
    struct AudioReception {
        std::mutex mutex;
        bool accepting = true;
        livekit::TrackSource source = livekit::TrackSource::Unknown;
        std::uint64_t generation = 0, frames = 0, sequence = 0;
        int sample_rate = 0, channels = 0;
        std::uint64_t window_sample_frames = 0, window_frames = 0;
        long double squares = 0;
        std::array<double, 3> coefficients{}, previous{}, previous2{};

        void ResetWindow() {
            window_sample_frames = window_frames = 0;
            squares = 0;
            previous.fill(0); previous2.fill(0);
        }
        bool Accumulate(const livekit::AudioFrame& frame) {
            const int rate = frame.sampleRate(), count = frame.numChannels(), per_channel = frame.samplesPerChannel();
            if (rate < 8000 || rate > 96000 || count < 1 || count > 8 || per_channel < 1 ||
                per_channel > rate || frame.data().size() != static_cast<std::size_t>(count) * per_channel) return false;
            if (rate != sample_rate || count != channels) {
                sample_rate = rate; channels = count; ResetWindow();
                constexpr std::array<double, 3> frequencies{440.0, 880.0, 1000.0};
                for (std::size_t i = 0; i < coefficients.size(); ++i)
                    coefficients[i] = 2.0 * std::cos(2.0 * std::numbers::pi * frequencies[i] / rate);
            }
            ++frames;
            bool counted = false;
            for (int i = 0; i < per_channel; ++i) {
                if (!counted) { ++window_frames; counted = true; }
                double mono = 0;
                for (int channel = 0; channel < count; ++channel) {
                    const double sample = frame.data()[static_cast<std::size_t>(i) * count + channel];
                    squares += sample * sample;
                    mono += sample;
                }
                mono /= count;
                for (std::size_t frequency = 0; frequency < previous.size(); ++frequency) {
                    const auto current = mono + coefficients[frequency] * previous[frequency] - previous2[frequency];
                    previous2[frequency] = previous[frequency]; previous[frequency] = current;
                }
                if (++window_sample_frames == static_cast<std::uint64_t>(rate)) {
                    ++sequence;
                    AudioDiagnostic("checkpoint", *this);
                    ResetWindow(); counted = false;
                }
            }
            return true;
        }
        double Rms() const {
            const auto samples = window_sample_frames * channels;
            return samples ? std::sqrt(static_cast<double>(squares / samples)) : 0;
        }
        double Power(std::size_t frequency) const {
            if (!window_sample_frames) return 0;
            const double value = previous[frequency] * previous[frequency] + previous2[frequency] * previous2[frequency] -
                coefficients[frequency] * previous[frequency] * previous2[frequency];
            return std::max(0.0, 2.0 * value / (static_cast<double>(window_sample_frames) * window_sample_frames));
        }
    };
    static void AudioDiagnostic(const char* event, const AudioReception& audio) {
        const char* source = audio.source == livekit::TrackSource::Microphone ? "microphone"
            : audio.source == livekit::TrackSource::ScreenShareAudio ? "screen_share_audio" : nullptr;
        if (!source) return; // An unknown declaration is never microphone proof.
        std::osyncstream(std::cout) << std::fixed << std::setprecision(6)
            << "[AUDIO SOURCE DIAG] source=" << source << " event=" << event
            << " generation=" << audio.generation << " frames=" << audio.frames << " sequence=" << audio.sequence
            << " sample_rate=" << audio.sample_rate << " channels=" << audio.channels
            << " window_frames=" << audio.window_frames << " samples=" << audio.window_sample_frames * audio.channels
            << " rms=" << audio.Rms() << " power440=" << audio.Power(0) << " power880=" << audio.Power(1)
            << " power1000=" << audio.Power(2) << '\n';
    }
    static void RetireAudio(const std::shared_ptr<AudioReception>& audio) {
        std::lock_guard lock(audio->mutex);
        if (!audio->accepting) return;
        audio->accepting = false;
        AudioDiagnostic("unsubscribed", *audio);
    }
    void RetireAllAudio() {
        std::lock_guard lock(audio_subscriptions_mutex_);
        for (const auto& [sid, subscription] : audio_subscriptions_) RetireAudio(subscription.reception);
        audio_subscriptions_.clear();
    }
    struct AudioSubscription {
        std::string participant_sid;
        std::weak_ptr<livekit::Track> track;
        std::shared_ptr<AudioReception> reception;
    };
    struct ScreenReception {
        std::mutex mutex;
        bool enabled = false;
        bool accepting = true;
        uint64_t sequence = 0;
        uint64_t frames = 0;
        int width = 0;
        int height = 0;
    };
    static void ScreenDiagnostic(const char* event, const ScreenReception& screen) {
        std::osyncstream(std::cout) << "[SCREEN SHARE DIAG] event=" << event
            << " frames=" << screen.frames << " sequence=" << screen.sequence
            << " width=" << screen.width << " height=" << screen.height << '\n';
    }
    struct VideoSubscription {
        std::string participant_sid;
        livekit::Track::I420VideoFrameSubscription token;
        std::shared_ptr<ScreenReception> screen;
    };
    std::mutex video_subscriptions_mutex_;
    std::map<std::string, VideoSubscription> video_subscriptions_;
    std::mutex audio_subscriptions_mutex_;
    std::map<std::weak_ptr<livekit::Track>, unsigned, std::owner_less<>> audio_sink_tracks_;
    std::map<std::string, AudioSubscription> audio_subscriptions_;
    std::shared_ptr<ReceptionState> reception_state_ = std::make_shared<ReceptionState>();
};

void PrintUsage(const char* prog_name) {
    std::cout << "LiveKit C++ SimpleRoom Example App\n\n";
    std::cout << "Usage:\n";
    std::cout << "  " << prog_name << " --url <livekit_ws_url> --token <access_token> [options]\n";
    std::cout << "  or set environment variables: LIVEKIT_URL, LIVEKIT_TOKEN, LIVEKIT_E2EE_KEY\n\n";
    std::cout << "Options:\n";
    std::cout << "  -u, --url <URL>            LiveKit WebSocket URL (e.g. wss://my-project.livekit.cloud)\n";
    std::cout << "  -t, --token <TOKEN>        LiveKit Room Access Token (JWT)\n";
    std::cout << "  --enable_e2ee              Enable End-to-End Encryption (E2EE)\n";
    std::cout << "  --e2ee_key <KEY>           Shared E2EE passphrase / encryption key\n";
    std::cout << "  --no_audio                 Disable local audio track publishing\n";
    std::cout << "  --no_video                 Disable local video track publishing\n";
    std::cout << "  -h, --help                 Show this help menu\n\n";
    std::cout << "Example Token Generation using LiveKit CLI (lk):\n";
    std::cout << "  lk token create --api-key <KEY> --api-secret <SECRET> --join --room simple-room --identity cpp-user\n";
}

std::string MaskToken(const std::string& token) {
    (void)token;
    return livekit::secure_log::SecretSummary();
}

int main(int argc, char* argv[]) {
    std::string url;
    std::string token;
    bool enable_e2ee = false;
    std::string e2ee_key;
    bool publish_audio = true;
    bool publish_video = true;

    // 1. 从环境变量读取默认值
    const char* env_url = std::getenv("LIVEKIT_URL");
    const char* env_token = std::getenv("LIVEKIT_TOKEN");
    const char* env_e2ee_key = std::getenv("LIVEKIT_E2EE_KEY");
    if (env_url) url = env_url;
    if (env_token) token = env_token;
    if (env_e2ee_key) {
        e2ee_key = env_e2ee_key;
        enable_e2ee = true;
    }

    // 2. 解析命令行参数（覆盖环境变量）
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-u" || arg == "--url") && i + 1 < argc) {
            url = argv[++i];
        } else if ((arg == "-t" || arg == "--token") && i + 1 < argc) {
            token = argv[++i];
        } else if (arg == "--enable_e2ee") {
            enable_e2ee = true;
        } else if (arg == "--e2ee_key" && i + 1 < argc) {
            e2ee_key = argv[++i];
            enable_e2ee = true;
        } else if (arg == "--no_audio" || arg == "--no-audio") {
            publish_audio = false;
        } else if (arg == "--no_video" || arg == "--no-video") {
            publish_video = false;
        } else if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        }
    }

    std::cout << "==================================================\n";
    std::cout << "       LiveKit C++ Client SDK - SimpleRoom        \n";
    std::cout << "==================================================\n";

    if (url.empty() || token.empty()) {
        std::cout << "[NOTICE] Required arguments --url or --token not provided.\n";
        PrintUsage(argv[0]);
        std::cout << "\n[INFO] Dry-run check completed successfully.\n";
        return 0;
    }

    std::cout << "[Config] LiveKit URL   : "
              << livekit::secure_log::EndpointSummary(url) << std::endl;
    std::cout << "[Config] Access Token  : " << MaskToken(token) << std::endl;
    std::cout << "[Config] Publish Audio : " << (publish_audio ? "ENABLED" : "DISABLED") << std::endl;
    std::cout << "[Config] Publish Video : " << (publish_video ? "ENABLED" : "DISABLED") << std::endl;
    std::cout << "[Config] E2EE Enabled  : " << (enable_e2ee ? "YES" : "NO") << std::endl;

    std::shared_ptr<livekit::diagnostic::DiagnosticPipeline> diagnostics;
    if (const char* diagnostic_root = std::getenv("COHAVORA_SAMPLE_DIAGNOSTIC_ROOT");
        diagnostic_root && *diagnostic_root) {
        diagnostics = std::make_shared<livekit::diagnostic::DiagnosticPipeline>();
        diagnostics->SetRetentionEnabled(false);
        diagnostics->SetMirror([](const livekit::diagnostic::Event& event) {
            using livekit::diagnostic::EventKind;
            if (event.kind == EventKind::RtcSdpStep) {
                std::cout << "[SDP DIAG] kind=" << livekit::diagnostic::EventName(event.kind)
                          << " role=" << livekit::diagnostic::SdpRoleName(event.sdp_role)
                          << " action=" << livekit::diagnostic::SdpActionName(event.sdp_action)
                          << " phase=" << livekit::diagnostic::SdpPhaseName(event.sdp_phase)
                          << " reason=" << livekit::diagnostic::SdpReasonName(event.sdp_reason)
                          << " before=" << livekit::diagnostic::SdpStateName(event.signaling_before)
                          << " after=" << livekit::diagnostic::SdpStateName(event.signaling_after)
                          << " description=" << livekit::diagnostic::SdpDescriptionName(event.sdp_description)
                          << " sequence=" << event.sdp_sequence
                          << " after_terminal=" << (event.sdp_after_terminal ? 1 : 0)
                          << " ice_restart=" << (event.sdp_ice_restart ? 1 : 0)
                          << " rtc_error_type=" << event.rtc_error_type
                          << " elapsed_ms=" << event.duration_ms << std::endl;
                return;
            }
            if (event.kind != EventKind::RoomConnectTerminal &&
                event.kind != EventKind::ReconnectEpisodeStarted &&
                event.kind != EventKind::ReconnectEpisodeTerminal &&
                event.kind != EventKind::ReconnectAttemptStarted &&
                event.kind != EventKind::ReconnectAttemptTerminal &&
                event.kind != EventKind::ReconnectModeChanged) return;
            std::cout << "[RECONNECT DIAG] kind=" << livekit::diagnostic::EventName(event.kind)
                      << " stage=" << livekit::diagnostic::StageName(event.stage)
                      << " error_code=" << livekit::diagnostic::ErrorCodeName(event.error_code)
                      << " outcome=" << livekit::diagnostic::OutcomeName(event.outcome)
                      << " attempt=" << event.attempt
                      << " elapsed_ms=" << event.duration_ms << std::endl;
        });
        if (diagnostics->StartWriter(diagnostic_root)) {
            livekit::diagnostic::InstallBusinessPipeline(diagnostics);
        } else {
            diagnostics.reset();
        }
    }

    asio::io_context io_ctx;

    std::atomic<bool> capture_running{true};
    std::thread audioThread;
    std::thread videoThread;

    if (!BindPrivatePlayoutRoute()) return 2;

    // 捕获 Ctrl+C 信号量
    asio::signal_set signals(io_ctx, SIGINT, SIGTERM);

    auto room = livekit::Room::Create(io_ctx.get_executor());
    auto listener = std::make_shared<SimpleRoomListener>();
    room->AddListener(listener);

    // 配置 E2EE (如果开启)
    if (enable_e2ee) {
        livekit::KeyProviderOptions kopts;
        kopts.shared_key = true;
        auto key_provider = std::make_shared<livekit::KeyProvider>(kopts);
        if (!e2ee_key.empty()) {
            std::vector<uint8_t> key_vec(e2ee_key.begin(), e2ee_key.end());
            key_provider->SetSharedKey(key_vec, 0);
        }

        livekit::E2eeOptions eopts;
        eopts.encryption_type = livekit::EncryptionType::GCM;
        eopts.key_provider = key_provider;
        room->EnableE2ee(eopts);
        std::cout << "[E2EE Engine] Initialized AES-256-GCM E2EE key provider." << std::endl;
    }

    signals.async_wait([room, &io_ctx, &capture_running, &audioThread, &videoThread](const std::error_code& error, int signal_number) {
        if (!error) {
            std::cout << "\n[SIGNAL] Received exit signal (" << signal_number << "), disconnecting simple_room..." << std::endl;
            capture_running.store(false);
            if (audioThread.joinable()) audioThread.join();
            if (videoThread.joinable()) videoThread.join();
            room->Disconnect();
            io_ctx.stop();
        }
    });

    livekit::SignalOptions opts;
    opts.auto_subscribe = true;
    opts.single_peer_connection = true;
    opts.connect_timeout = std::chrono::seconds(10);
    // Explicit opt-in for a private development service. Secure transport
    // remains the default; never infer this permission from the URL scheme.
    if (const char* allow_insecure = std::getenv("COHAVORA_SAMPLE_ALLOW_INSECURE_TRANSPORT");
        allow_insecure && std::string_view(allow_insecure) == "1") {
        opts.allow_insecure_transport = true;
    }
    if (const char* experiment = std::getenv("COHAVORA_SAMPLE_RECONNECT_TIMEOUT_EXPERIMENT");
        experiment && std::string_view(experiment) == "25-35-90") {
        opts.timeouts.negotiation = std::chrono::seconds(25);
        opts.timeouts.reconnect_attempt = std::chrono::seconds(35);
        opts.timeouts.reconnect_total = std::chrono::seconds(90);
        std::cout << "[Config] Reconnect timeout experiment: negotiation=25s, attempt=35s, total=90s" << std::endl;
    }

    asio::co_spawn(io_ctx, [room, url, token, opts, publish_audio, publish_video, &capture_running, &audioThread, &videoThread]() -> asio::awaitable<void> {
        try {
            std::cout << "[Connect] Connecting to room..." << std::endl;
            co_await room->ConnectAsync(url, token, opts);
            {
                auto lp = room->local_participant();
                std::cout << "[State] Connected! Local Participant Identity: "
                          << (lp ? lp->identity() : "N/A")
                          << ", SID: " << (lp ? lp->sid() : "N/A") << std::endl;

                auto remotes = room->remote_participants();
                std::cout << "[State] Remote Participants count: " << remotes.size() << std::endl;
                for (const auto& [sid, p] : remotes) {
                    std::cout << "  - Remote Identity: " << p->identity() << " (sid: " << sid << ")" << std::endl;
                }

                if (lp) {
                    // 发布本地音频 Track
                    if (publish_audio) {
                        auto audio_source = std::make_shared<livekit::AudioSource>(48000, 1, 10);
                        auto audio_track = livekit::LocalAudioTrack::createLocalAudioTrack("simple_audio", audio_source);
                        audio_track->set_source(livekit::TrackSource::Microphone);
                        co_await lp->PublishTrackAsync(audio_track);
                        std::cout << "[Media] Local Audio Track published ('simple_audio')." << std::endl;

                        audioThread = std::thread([audio_source, &capture_running]() {
                            RunAudioCaptureLoop(audio_source, capture_running);
                        });
                    }

                    // 发布本地视频 Track
                    if (publish_video) {
                        auto video_source = std::make_shared<livekit::VideoSource>(1280, 720);
                        auto video_track = livekit::LocalVideoTrack::createLocalVideoTrack("simple_video", video_source);
                        co_await lp->PublishTrackAsync(video_track);
                        std::cout << "[Media] Local Video Track published ('simple_video')." << std::endl;

                        videoThread = std::thread([video_source, &capture_running]() {
                            RunVideoCaptureLoop(video_source, capture_running);
                        });
                    }
                }

                std::cout << "\n[Running] SimpleRoom is active. Press Ctrl+C to disconnect and exit." << std::endl;
            }
        } catch (const std::exception&) {
            std::cerr << "[EXCEPTION] Connection exception: "
                      << livekit::secure_log::ExceptionSummary("simple_room_connect")
                      << std::endl;
        }
    }, asio::detached);

    io_ctx.run();

    capture_running.store(false);
    if (audioThread.joinable()) audioThread.join();
    if (videoThread.joinable()) videoThread.join();

    if (diagnostics) {
        livekit::diagnostic::InstallBusinessPipeline({});
        diagnostics->Close();
    }
    std::cout << "[SimpleRoom] Disconnected. Exit completed." << std::endl;
    return 0;
}
