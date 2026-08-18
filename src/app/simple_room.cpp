#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <thread>
#include <atomic>
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
#include "frame_cryptor.h"

// 产生 48kHz 单声道模拟音频白噪声/正弦采样帧
void RunAudioCaptureLoop(std::shared_ptr<livekit::AudioSource> source, std::atomic<bool>& running) {
    auto frame = livekit::AudioFrame::create(48000, 1, 480); // 10ms @ 48kHz (480 采样点)
    int step = 0;
    while (running.load()) {
        for (size_t i = 0; i < frame.data().size(); ++i) {
            frame.data()[i] = static_cast<int16_t>((step + i) % 2000 - 1000);
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

// SimpleRoom 核心事件监听器
class SimpleRoomListener : public livekit::RoomListener {
public:
    void OnConnected() override {
        std::cout << "\n==================================================" << std::endl;
        std::cout << "[SUCCESS] SimpleRoom::OnConnected - Successfully connected to LiveKit Room!" << std::endl;
        std::cout << "==================================================" << std::endl;
    }

    void OnDisconnected(const std::string& reason) override {
        std::cout << "\n[INFO] SimpleRoom::OnDisconnected - Reason: "
                  << (reason.empty() ? "Normal Disconnect" : reason) << std::endl;
    }

    void OnReconnecting() override {
        std::cout << "[WARN] SimpleRoom::OnReconnecting - Network unstable, reconnecting..." << std::endl;
    }

    void OnReconnected() override {
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
        }
    }

    void OnTrackUnsubscribed(std::shared_ptr<livekit::Track> track,
                             std::shared_ptr<livekit::TrackPublication> publication,
                             std::shared_ptr<livekit::RemoteParticipant> participant) override {
        if (participant && publication) {
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

    void OnActiveSpeakersChanged(const std::vector<std::shared_ptr<livekit::Participant>>& speakers) override {
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
    if (token.length() <= 12) return "***";
    return token.substr(0, 6) + "..." + token.substr(token.length() - 6);
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

    std::cout << "[Config] LiveKit URL   : " << url << std::endl;
    std::cout << "[Config] Access Token  : " << MaskToken(token) << std::endl;
    std::cout << "[Config] Publish Audio : " << (publish_audio ? "ENABLED" : "DISABLED") << std::endl;
    std::cout << "[Config] Publish Video : " << (publish_video ? "ENABLED" : "DISABLED") << std::endl;
    std::cout << "[Config] E2EE Enabled  : " << (enable_e2ee ? "YES" : "NO") << std::endl;

    asio::io_context io_ctx;

    std::atomic<bool> capture_running{true};
    std::thread audioThread;
    std::thread videoThread;

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

    asio::co_spawn(io_ctx, [room, url, token, opts, publish_audio, publish_video, &capture_running, &audioThread, &videoThread]() -> asio::awaitable<void> {
        try {
            std::cout << "[Connect] Connecting to room..." << std::endl;
            bool success = co_await room->Connect(url, token, opts);
            if (success) {
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
                        lp->PublishTrack(audio_track);
                        std::cout << "[Media] Local Audio Track published ('simple_audio')." << std::endl;

                        audioThread = std::thread([audio_source, &capture_running]() {
                            RunAudioCaptureLoop(audio_source, capture_running);
                        });
                    }

                    // 发布本地视频 Track
                    if (publish_video) {
                        auto video_source = std::make_shared<livekit::VideoSource>(1280, 720);
                        auto video_track = livekit::LocalVideoTrack::createLocalVideoTrack("simple_video", video_source);
                        lp->PublishTrack(video_track);
                        std::cout << "[Media] Local Video Track published ('simple_video')." << std::endl;

                        videoThread = std::thread([video_source, &capture_running]() {
                            RunVideoCaptureLoop(video_source, capture_running);
                        });
                    }
                }

                std::cout << "\n[Running] SimpleRoom is active. Press Ctrl+C to disconnect and exit." << std::endl;
            } else {
                std::cerr << "[ERROR] Room::Connect returned false." << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[EXCEPTION] Connection exception: " << e.what() << std::endl;
        }
    }, asio::detached);

    io_ctx.run();

    capture_running.store(false);
    if (audioThread.joinable()) audioThread.join();
    if (videoThread.joinable()) videoThread.join();

    std::cout << "[SimpleRoom] Disconnected. Exit completed." << std::endl;
    return 0;
}
