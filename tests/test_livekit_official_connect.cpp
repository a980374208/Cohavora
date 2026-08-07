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

// 仿照 basic_room 产生模拟音频白噪声帧
void RunNoiseCaptureLoop(std::shared_ptr<livekit::AudioSource> source, std::atomic<bool>& running) {
    auto frame = livekit::AudioFrame::create(48000, 1, 480); // 10ms @ 48kHz 单声道 (480 采样点)
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

// 产生全屏 红/绿/蓝/黑 轮播色块的 RGBA 视频帧 (1280x720 @ 30FPS)
void RunFakeVideoCaptureLoop(std::shared_ptr<livekit::VideoSource> source, std::atomic<bool>& running) {
    const int width = 1280;
    const int height = 720;
    auto frame = livekit::VideoFrame::create(width, height, livekit::VideoBufferType::RGBA);
    
    struct ColorRGBA { uint8_t r, g, b, a; const char* name; };
    const ColorRGBA color_palette[] = {
        {255, 0, 0, 255, "RED (红色)"},
        {0, 255, 0, 255, "GREEN (绿色)"},
        {0, 0, 255, 255, "BLUE (蓝色)"},
        {0, 0, 0, 255, "BLACK (黑色)"}
    };
    const size_t num_colors = sizeof(color_palette) / sizeof(color_palette[0]);

    int frame_count = 0;
    size_t color_idx = 0;

    std::cout << "[Video Stream] Starting RGBA color carousel (Red -> Green -> Blue -> Black @ 30FPS)..." << std::endl;

    while (running.load()) {
        // 每 30 帧 (约 1 秒) 切换一次色块
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

// 官方/真实 LiveKit 服务器连接测试监听器
class OfficialServerListener : public livekit::RoomListener {
public:
    void OnConnected() override {
        std::cout << "\n==================================================" << std::endl;
        std::cout << "[SUCCESS] Room::OnConnected - Successfully connected to LiveKit Server!" << std::endl;
        std::cout << "==================================================" << std::endl;
    }

    void OnDisconnected(const std::string& reason) override {
        std::cout << "\n[INFO] Room::OnDisconnected - Reason: " << (reason.empty() ? "Normal Disconnect" : reason) << std::endl;
    }

    void OnReconnecting() override {
        std::cout << "[INFO] Room::OnReconnecting - Network weak, trying to reconnect..." << std::endl;
    }

    void OnReconnected() override {
        std::cout << "[INFO] Room::OnReconnected - Connection restored!" << std::endl;
    }

    void OnParticipantConnected(std::shared_ptr<livekit::RemoteParticipant> participant) override {
        if (participant) {
            std::cout << "[EVENT] Remote Participant Joined: identity=" << participant->identity()
                      << ", sid=" << participant->sid() << std::endl;
        }
    }

    void OnParticipantDisconnected(std::shared_ptr<livekit::RemoteParticipant> participant) override {
        if (participant) {
            std::cout << "[EVENT] Remote Participant Left: identity=" << participant->identity()
                      << ", sid=" << participant->sid() << std::endl;
        }
    }

    void OnTrackPublished(std::shared_ptr<livekit::RemoteParticipant> participant, std::shared_ptr<livekit::TrackPublication> publication) override {
        if (participant && publication) {
            std::string kind_str = (publication->track() && publication->track()->kind() == livekit::TrackKind::Audio) ? "AUDIO" : "VIDEO";
            std::cout << "[TRACK PUBLISHED] Remote user [" << participant->identity()
                      << "] published " << kind_str << " track: name='" << publication->name()
                      << "', sid=" << publication->sid() << std::endl;
        }
    }

    void OnTrackSubscribed(std::shared_ptr<livekit::Track> track, std::shared_ptr<livekit::TrackPublication> publication, std::shared_ptr<livekit::RemoteParticipant> participant) override {
        if (participant && publication) {
            std::string kind_str = (track && track->kind() == livekit::TrackKind::Audio) ? "AUDIO" : "VIDEO";
            std::cout << "[TRACK SUBSCRIBED] Successfully subscribed to " << kind_str << " track from user ["
                      << participant->identity() << "]: name='" << publication->name()
                      << "', track_sid=" << publication->sid() << std::endl;
        }
    }

    void OnTrackMuted(std::shared_ptr<livekit::Participant> participant, std::shared_ptr<livekit::TrackPublication> publication, bool muted) override {
        if (participant && publication) {
            std::cout << "[TRACK MUTE EVENT] User [" << participant->identity() << "] track '"
                      << publication->name() << "' is now " << (muted ? "MUTED (静音/禁用)" : "UNMUTED (恢复播放)") << std::endl;
        }
    }

    void OnDataReceived(const std::vector<uint8_t>& payload, std::shared_ptr<livekit::RemoteParticipant> participant, const std::string& topic) override {
        if (topic == "lk.chat" || topic == "lk-chat-topic") {
            return; // 聊天内容由 OnChatMessage 格式化高亮打印
        }
        std::string sender = participant ? participant->identity() : "System/Unknown";
        std::string text(payload.begin(), payload.end());
        std::cout << "[RECV DATA] Received custom data packet from user [" << sender << "] on topic '" << topic
                  << "', size=" << payload.size() << " bytes: " << text << std::endl;
    }

    void OnChatMessage(const livekit::ChatMessage& message, std::shared_ptr<livekit::Participant> participant) override {
        std::string sender = participant ? participant->identity() : message.sender_identity;
        std::cout << "[RECV CHAT] Chat message from user [" << sender << "]: " << message.message
                  << " (message_id: " << message.id << ")" << std::endl;
    }
};

void PrintUsage(const char* prog_name) {
    std::cout << "Usage:\n";
    std::cout << "  " << prog_name << " --url <livekit_ws_url> --token <access_token>\n";
    std::cout << "  or set environment variables LIVEKIT_URL and LIVEKIT_TOKEN\n\n";
    std::cout << "Options:\n";
    std::cout << "  -u, --url <URL>      LiveKit WebSocket URL (e.g. wss://my-project.livekit.cloud)\n";
    std::cout << "  -t, --token <TOKEN>  LiveKit Room Access Token (JWT)\n";
    std::cout << "  -h, --help           Show this help message\n\n";
    std::cout << "Example using LiveKit CLI (lk) to generate a token:\n";
    std::cout << "  lk token create --api-key <KEY> --api-secret <SECRET> --join --room test-room --identity test-user\n";
}

std::string MaskToken(const std::string& token) {
    if (token.length() <= 12) return "***";
    return token.substr(0, 6) + "..." + token.substr(token.length() - 6);
}

int main(int argc, char* argv[]) {
    std::string url;
    std::string token;

    // 1. 尝试从环境变量读取
    const char* env_url = std::getenv("LIVEKIT_URL");
    const char* env_token = std::getenv("LIVEKIT_TOKEN");
    if (env_url) url = env_url;
    if (env_token) token = env_token;

    // 2. 解析命令行参数（可覆盖环境变量）
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-u" || arg == "--url") && i + 1 < argc) {
            url = argv[++i];
        } else if ((arg == "-t" || arg == "--token") && i + 1 < argc) {
            token = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        }
    }

    std::cout << "==================================================\n";
    std::cout << "        LiveKit Official Server Connect Test       \n";
    std::cout << "==================================================\n";

    if (url.empty() || token.empty()) {
        std::cout << "[NOTICE] Connection URL or Access Token not specified.\n";
        PrintUsage(argv[0]);
        std::cout << "\n[INFO] Test completed in dry-run mode (argument parsing passed).\n";
        return 0;
    }

    std::cout << "[Config] Server URL : " << url << std::endl;
    std::cout << "[Config] Access Token: " << MaskToken(token) << std::endl;
    std::cout << "[Status] Connecting to server..." << std::endl;

    asio::io_context io_ctx;

    std::atomic<bool> capture_running{true};
    std::thread audioThread;
    std::thread videoThread;

    // 监听 Ctrl+C 信号以优雅退出
    asio::signal_set signals(io_ctx, SIGINT, SIGTERM);

    auto room = livekit::Room::Create(io_ctx.get_executor());
    auto listener = std::make_shared<OfficialServerListener>();
    room->AddListener(listener);

    signals.async_wait([room, &io_ctx, &capture_running, &audioThread, &videoThread](const std::error_code& error, int signal_number) {
        if (!error) {
            std::cout << "\n[SIGNAL] Received signal " << signal_number << ", stopping capture threads and disconnecting room..." << std::endl;
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

    asio::co_spawn(io_ctx, [room, url, token, opts, &capture_running, &audioThread, &videoThread]() -> asio::awaitable<void> {
        try {
            bool success = co_await room->Connect(url, token, opts);
            if (success) {
                auto lp = room->local_participant();
                std::cout << "[Connected] Local Participant SID: "
                          << (lp ? lp->sid() : "N/A")
                          << ", Identity: "
                          << (lp ? lp->identity() : "N/A")
                          << std::endl;

                auto remotes = room->remote_participants();
                std::cout << "[Room State] Existing Remote Participants count: " << remotes.size() << std::endl;
                for (const auto& [sid, p] : remotes) {
                    std::cout << "  - Identity: " << p->identity() << " (sid: " << sid << ")" << std::endl;
                }

                // ---- 仿照 basic_room 创建并发布音频和视频 Track ----
                if (lp) {
                    std::cout << "\n[Media] Creating local Audio & Video sources and tracks..." << std::endl;

                    // 1. 创建音频 Source (48kHz, 单声道) & Track ("noise")
                    auto audio_source = std::make_shared<livekit::AudioSource>(48000, 1, 10);
                    auto audio_track = livekit::LocalAudioTrack::createLocalAudioTrack("noise", audio_source);
                    lp->PublishTrack(audio_track);
                    std::cout << "[Media] -> Published Local Audio Track: name='noise'" << std::endl;

                    // 2. 创建视频 Source (1280x720) & Track ("rgb")
                    auto video_source = std::make_shared<livekit::VideoSource>(1280, 720);
                    auto video_track = livekit::LocalVideoTrack::createLocalVideoTrack("rgb", video_source);
                    lp->PublishTrack(video_track);
                    std::cout << "[Media] -> Published Local Video Track: name='rgb'" << std::endl;

                    // 3. 启动后台模拟音视频采集采样线程 (仿照 runNoiseCaptureLoop 和 runFakeVideoCaptureLoop)
                    audioThread = std::thread([audio_source, &capture_running]() {
                        RunNoiseCaptureLoop(audio_source, capture_running);
                    });

                    videoThread = std::thread([video_source, &capture_running]() {
                        RunFakeVideoCaptureLoop(video_source, capture_running);
                    });

                    std::cout << "[Media] -> Background audio (noise) and video (RGB 30fps) capture threads started." << std::endl;
                }

                std::cout << "\n[Running] LiveKit connection active with published Audio & Video tracks. Press Ctrl+C to exit." << std::endl;
            } else {
                std::cerr << "[ERROR] Failed to connect to LiveKit server (Connect returned false)." << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[EXCEPTION] Connection threw exception: " << e.what() << std::endl;
        }
    }, asio::detached);

    io_ctx.run();

    capture_running.store(false);
    if (audioThread.joinable()) audioThread.join();
    if (videoThread.joinable()) videoThread.join();

    std::cout << "[Finished] Exited connection test." << std::endl;
    return 0;
}
