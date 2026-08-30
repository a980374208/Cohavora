#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <asio.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <thread>
#include <atomic>
#include <sstream>
#include <iomanip>

#include "room.h"
#include "participant.h"
#include "chat_message.h"
#include "audio_source.h"
#include "video_source.h"
#include "local_audio_track.h"
#include "local_video_track.h"
#include "wasapi_types.h"
#include "wasapi_enumerator.h"
#include "wasapi_capture.h"
#include "dshow_types.h"
#include "dshow_enumerator.h"
#include "dshow_capture.h"
#include "media_converters.h"
#include "stats.h"
#include "telemetry.h"
#include "audio_vad.h"

// 房间事件监听器
class BroadcasterRoomListener : public livekit::RoomListener {
public:
    void OnConnected() override {
        std::cout << "\n===============================================================\n";
        std::cout << " [SUCCESS] Connected to LiveKit Room! Ready to publish stream! \n";
        std::cout << "===============================================================\n" << std::endl;
    }

    void OnDisconnected(const std::string& reason) override {
        std::cout << "\n[DISCONNECTED] Room disconnected. Reason: "
                  << (reason.empty() ? "Normal Disconnect" : reason) << std::endl;
    }

    void OnReconnecting() override {
        std::cout << "[WARN] Network unstable, trying to reconnect to room..." << std::endl;
    }

    void OnReconnected() override {
        std::cout << "[SUCCESS] Room reconnected successfully!" << std::endl;
    }

    void OnParticipantConnected(std::shared_ptr<livekit::RemoteParticipant> participant) override {
        if (participant) {
            std::cout << "[EVENT] Remote Participant Joined: " << participant->identity()
                      << " (SID: " << participant->sid() << ")" << std::endl;
        }
    }

    void OnParticipantDisconnected(std::shared_ptr<livekit::RemoteParticipant> participant) override {
        if (participant) {
            std::cout << "[EVENT] Remote Participant Left: " << participant->identity() << std::endl;
        }
    }

    void OnParticipantAttributesChanged(const std::map<std::string, std::string>& changed_attributes, std::shared_ptr<livekit::Participant> participant) override {
        std::string identity = participant ? participant->identity() : "Unknown";
        std::cout << "\n[ATTRIBUTES] Participant '" << identity << "' attributes updated:\n";
        for (const auto& kv : changed_attributes) {
            std::cout << "  * " << kv.first << " = '" << kv.second << "'\n";
        }
        std::cout << std::endl;
    }

    void OnParticipantPermissionsChanged(const livekit::ParticipantPermission& old_perm, const livekit::ParticipantPermission& new_perm, std::shared_ptr<livekit::Participant> participant) override {
        std::string identity = participant ? participant->identity() : "Unknown";
        std::cout << "[PERMISSION] Participant '" << identity << "' permissions updated: "
                  << "canPublish=" << (new_perm.can_publish ? "true" : "false")
                  << ", canPublishData=" << (new_perm.can_publish_data ? "true" : "false")
                  << ", canSubscribe=" << (new_perm.can_subscribe ? "true" : "false") << std::endl;
    }

    void OnTrackPublished(std::shared_ptr<livekit::RemoteParticipant> participant,
                          std::shared_ptr<livekit::TrackPublication> publication) override {
        if (participant && publication) {
            std::cout << "[REMOTE TRACK] " << participant->identity() << " published track: "
                      << publication->name() << " (SID: " << publication->sid() << ")" << std::endl;
        }
    }

    void OnChatMessage(const livekit::ChatMessage& message, std::shared_ptr<livekit::Participant> participant) override {
        std::string sender = participant ? participant->identity() : message.sender_identity;
        std::cout << "\n[CHAT] " << sender << ": " << message.message << std::endl;
    }

    void OnRoomStats(const livekit::RoomStatsReport& report) override {
        std::cout << "\n[RTC STATS] Pub RTT: " << report.publisher_rtt_ms << "ms, Sub RTT: "
                  << report.subscriber_rtt_ms << "ms, Total Sent: "
                  << (report.total_bytes_sent / 1024) << " KB, Bitrate: "
                  << (report.available_outgoing_bitrate / 1000.0) << " kbps" << std::endl;
    }

    void OnActiveSpeakersChanged(const std::vector<std::shared_ptr<livekit::Participant>>& speakers) override {
        if (speakers.empty()) return;
        std::cout << "\n-------------------------------------------------------------------" << std::endl;
        std::cout << " [SPEAKER DETECT] Active Speakers (" << speakers.size() << " active):" << std::endl;
        for (size_t i = 0; i < speakers.size(); ++i) {
            float pct = speakers[i]->audio_level() * 100.0f;
            std::cout << "  * [" << (i + 1) << "] " << speakers[i]->identity()
                      << " (SID: " << speakers[i]->sid() << ")"
                      << " | Audio Level: " << std::fixed << std::setprecision(1) << pct << "%"
                      << (speakers[i]->is_speaking() ? " [SPEAKING]" : "") << std::endl;
        }
        std::cout << "-------------------------------------------------------------------\n" << std::endl;
    }
};

void PrintUsage(const char* prog_name) {
    std::cout << "\n===================================================================\n";
    std::cout << "         LiveKit Windows Media Broadcaster (WASAPI + DirectShow)   \n";
    std::cout << "===================================================================\n";
    std::cout << "Usage:\n";
    std::cout << "  " << prog_name << " [options]\n\n";
    std::cout << "Connection Options:\n";
    std::cout << "  -u, --url <URL>              LiveKit WebSocket URL (e.g. wss://my-project.livekit.cloud)\n";
    std::cout << "  -t, --token <TOKEN>          Room Access Token (JWT)\n\n";
    std::cout << "Capture Options:\n";
    std::cout << "  -a, --audio-source <type>    Audio capture mode: 'mic' (Microphone), 'speaker' (Loopback), or 'none' (default: mic)\n";
    std::cout << "  --audio-device <ID/Name>     Specific audio device ID or Name\n";
    std::cout << "  -v, --video-source <type>    Video capture mode: 'camera' (Webcam/OBS Virtual Cam) or 'none' (default: camera)\n";
    std::cout << "  --video-device <Path/Name>   Specific DirectShow device path or Name\n";
    std::cout << "  --width <W>                  Desired video width (default: 1280)\n";
    std::cout << "  --height <H>                 Desired video height (default: 720)\n";
    std::cout << "  --fps <FPS>                  Desired video frame rate (default: 30)\n\n";
    std::cout << "Utility Options:\n";
    std::cout << "  -l, --list-devices           List all available audio and video devices with capabilities\n";
    std::cout << "  -h, --help                   Show this help message\n\n";
    std::cout << "Environment Variables:\n";
    std::cout << "  LIVEKIT_URL                  Default fallback WebSocket URL\n";
    std::cout << "  LIVEKIT_TOKEN                Default fallback Access Token\n\n";
}

void ListAllDevices() {
    std::cout << "\n==================================================\n";
    std::cout << "       System Audio & Video Capture Devices       \n";
    std::cout << "==================================================\n";

    std::cout << "\n[1] WASAPI Audio Input Devices (Microphones):\n";
    auto mics = livekit::WasapiEnumerator::EnumerateInputDevices();
    if (mics.empty()) {
        std::cout << "  (No microphone devices found)\n";
    } else {
        for (size_t i = 0; i < mics.size(); ++i) {
            std::cout << "  [" << i << "] " << mics[i].name << (mics[i].is_default ? " [DEFAULT]" : "")
                      << "\n      ID: " << mics[i].id
                      << "\n      Format: " << mics[i].default_sample_rate << "Hz, "
                      << mics[i].default_channels << " channels\n";
        }
    }

    std::cout << "\n[2] WASAPI Audio Output Devices (Speakers / Loopback):\n";
    auto speakers = livekit::WasapiEnumerator::EnumerateOutputDevices();
    if (speakers.empty()) {
        std::cout << "  (No speaker output devices found)\n";
    } else {
        for (size_t i = 0; i < speakers.size(); ++i) {
            std::cout << "  [" << i << "] " << speakers[i].name << (speakers[i].is_default ? " [DEFAULT]" : "")
                      << "\n      ID: " << speakers[i].id
                      << "\n      Format: " << speakers[i].default_sample_rate << "Hz, "
                      << speakers[i].default_channels << " channels\n";
        }
    }

    std::cout << "\n[3] DirectShow Video Devices (Cameras / OBS Virtual Cam):\n";
    auto cameras = livekit::DShowEnumerator::EnumerateVideoDevices();
    if (cameras.empty()) {
        std::cout << "  (No DirectShow video devices found)\n";
    } else {
        for (size_t i = 0; i < cameras.size(); ++i) {
            std::cout << "  [" << i << "] " << cameras[i].name << (cameras[i].is_default ? " [DEFAULT]" : "")
                      << "\n      Path: " << cameras[i].path
                      << "\n      Capabilities: " << cameras[i].capabilities.size() << " formats\n";
            for (size_t c = 0; c < std::min<size_t>(cameras[i].capabilities.size(), 4); ++c) {
                const auto& cap = cameras[i].capabilities[c];
                std::cout << "        -> " << cap.width << "x" << cap.height << " @" << cap.max_fps
                          << "fps [" << livekit::MediaConverters::PixelFormatToString(cap.format) << "]\n";
            }
            if (cameras[i].capabilities.size() > 4) {
                std::cout << "        ... (and " << (cameras[i].capabilities.size() - 4) << " more resolutions)\n";
            }
        }
    }
    std::cout << "\n==================================================\n\n";
}

int main(int argc, char* argv[]) {
    // 保护 COM 生命周期
    struct ScopedCom {
        HRESULT hr;
        ScopedCom() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
        ~ScopedCom() {
            if (SUCCEEDED(hr)) CoUninitialize();
        }
    } com_scope;

    std::string url;
    std::string token;
    std::string audio_mode = "mic";      // mic, speaker, none
    std::string video_mode = "camera";   // camera, none
    std::string audio_dev_id;
    std::string video_dev_path;
    int video_width = 0;
    int video_height = 0;
    int video_fps = 0;

    // 1. 读取环境变量
    if (const char* env_url = std::getenv("LIVEKIT_URL")) url = env_url;
    if (const char* env_token = std::getenv("LIVEKIT_TOKEN")) token = env_token;

    // 2. 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            return 0;
        } else if (arg == "-l" || arg == "--list-devices") {
            ListAllDevices();
            return 0;
        } else if ((arg == "-u" || arg == "--url") && i + 1 < argc) {
            url = argv[++i];
        } else if ((arg == "-t" || arg == "--token") && i + 1 < argc) {
            token = argv[++i];
        } else if ((arg == "-a" || arg == "--audio-source") && i + 1 < argc) {
            audio_mode = argv[++i];
        } else if (arg == "--audio-device" && i + 1 < argc) {
            audio_dev_id = argv[++i];
        } else if ((arg == "-v" || arg == "--video-source") && i + 1 < argc) {
            video_mode = argv[++i];
        } else if (arg == "--video-device" && i + 1 < argc) {
            video_dev_path = argv[++i];
        } else if (arg == "--width" && i + 1 < argc) {
            video_width = std::stoi(argv[++i]);
        } else if (arg == "--height" && i + 1 < argc) {
            video_height = std::stoi(argv[++i]);
        } else if (arg == "--fps" && i + 1 < argc) {
            video_fps = std::stoi(argv[++i]);
        }
    }

    if (url.empty() || token.empty()) {
        std::cout << "\n[ERROR] LiveKit WebSocket URL or Access Token is missing!\n";
        PrintUsage(argv[0]);
        std::cout << "Quick Device Check:\n";
        ListAllDevices();
        return 1;
    }

    std::cout << "\n===================================================================\n";
    std::cout << "        Starting LiveKit Real-Time Media Broadcaster               \n";
    std::cout << "===================================================================\n";
    std::cout << "  Server URL:    " << url << "\n";
    std::cout << "  Audio Mode:    " << audio_mode << "\n";
    std::cout << "  Video Mode:    " << video_mode;
    if (video_width > 0 && video_height > 0) {
        std::cout << " (" << video_width << "x" << video_height << " @" << (video_fps > 0 ? video_fps : 30) << "fps)\n";
    } else {
        std::cout << " (Native Device Resolution / Auto Negotiate)\n";
    }
    std::cout << "===================================================================\n\n";

    asio::io_context io_ctx;
    auto room = livekit::Room::Create(io_ctx.get_executor());
    auto listener = std::make_shared<BroadcasterRoomListener>();
    room->AddListener(listener);

    // ------------------------------------------------------------------
    // Step 1: 创建本地音视频源与 Track
    // ------------------------------------------------------------------
    std::shared_ptr<livekit::AudioSource> audio_source;
    std::shared_ptr<livekit::LocalAudioTrack> local_audio_track;
    std::shared_ptr<livekit::WasapiAudioCapture> wasapi_cap;

    if (audio_mode != "none") {
        audio_source = std::make_shared<livekit::AudioSource>(48000, 2);
        local_audio_track = livekit::LocalAudioTrack::createLocalAudioTrack("wasapi_audio", audio_source);

        wasapi_cap = livekit::WasapiAudioCapture::Create();
        livekit::WasapiCaptureConfig acfg;
        acfg.type = (audio_mode == "speaker" || audio_mode == "loopback") ?
                    livekit::WasapiCaptureType::DesktopLoopback : livekit::WasapiCaptureType::Microphone;
        acfg.device_id = audio_dev_id;
        acfg.target_sample_rate = 48000;
        acfg.target_channels = 2;

        if (wasapi_cap->Init(acfg, audio_source)) {
            bool a_start = wasapi_cap->Start();
            std::cout << "[AUDIO CAPTURE] " << (acfg.type == livekit::WasapiCaptureType::Microphone ? "Microphone" : "Speaker Loopback")
                      << " capture started: " << (a_start ? "SUCCESS" : "NO ACTIVE DEVICE") << std::endl;
        } else {
            std::cout << "[AUDIO CAPTURE] Failed to initialize WASAPI capture!" << std::endl;
        }
    }

    std::shared_ptr<livekit::VideoSource> video_source;
    std::shared_ptr<livekit::LocalVideoTrack> local_video_track;
    std::shared_ptr<livekit::DShowVideoCapture> dshow_cap;
    std::thread pattern_thread;
    std::atomic<bool> pattern_running{false};

    if (video_mode == "pattern" || video_mode == "test") {
        int w = video_width > 0 ? video_width : 1280;
        int h = video_height > 0 ? video_height : 720;
        video_width = w;
        video_height = h;
        video_source = std::make_shared<livekit::VideoSource>(w, h);
        local_video_track = livekit::LocalVideoTrack::createLocalVideoTrack("dshow_video", video_source);

        pattern_running.store(true);
        pattern_thread = std::thread([video_source, w, h, &pattern_running]() {
            int frame_count = 0;
            auto start_time = std::chrono::steady_clock::now();
            while (pattern_running.load()) {
                livekit::VideoFrame frame = livekit::VideoFrame::create(w, h, livekit::VideoBufferType::RGBA);
                uint8_t* data = frame.data();
                
                int shift = (frame_count * 4) % w;
                // SMPTE 类似彩色横条与动态方块
                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        int idx = (y * w + x) * 4;
                        int col_sec = ((x + shift) * 7) / w;
                        uint8_t r = 0, g = 0, b = 0;
                        switch (col_sec % 7) {
                            case 0: r = 255; g = 255; b = 255; break; // White
                            case 1: r = 255; g = 255; b = 0;   break; // Yellow
                            case 2: r = 0;   g = 255; b = 255; break; // Cyan
                            case 3: r = 0;   g = 255; b = 0;   break; // Green
                            case 4: r = 255; g = 0;   b = 255; break; // Magenta
                            case 5: r = 255; g = 0;   b = 0;   break; // Red
                            case 6: r = 0;   g = 0;   b = 255; break; // Blue
                        }
                        // 底部画动态时间指示条
                        if (y > h * 4 / 5) {
                            int bar_x = (frame_count * 8) % w;
                            if (std::abs(x - bar_x) < 20) {
                                r = 255; g = 255; b = 255;
                            } else {
                                r = (x * 255) / w;
                                g = (y * 255) / h;
                                b = 128;
                            }
                        }
                        data[idx] = r;
                        data[idx + 1] = g;
                        data[idx + 2] = b;
                        data[idx + 3] = 255;
                    }
                }

                livekit::VideoCaptureOptions opts;
                opts.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                opts.rotation = livekit::VideoRotation::VIDEO_ROTATION_0;

                video_source->captureFrame(frame, opts);
                frame_count++;

                if (frame_count % 90 == 0) {
                    std::cout << "[VIDEO STATS] Generated & Pushed " << frame_count << " frames to WebRTC pipeline (" << w << "x" << h << " @30fps)" << std::endl;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 fps
            }
        });
        std::cout << "[VIDEO CAPTURE] Animated Test Pattern (Color Bars + Motion) started: SUCCESS (" << w << "x" << h << " @30fps)" << std::endl;
    } else if (video_mode != "none") {
        int init_w = (video_width > 0) ? video_width : 1280;
        int init_h = (video_height > 0) ? video_height : 720;
        video_source = std::make_shared<livekit::VideoSource>(init_w, init_h);
        local_video_track = livekit::LocalVideoTrack::createLocalVideoTrack("dshow_video", video_source);

        dshow_cap = livekit::DShowVideoCapture::Create();
        livekit::DShowCaptureConfig vcfg;
        vcfg.device_path = video_dev_path;
        vcfg.width = video_width;
        vcfg.height = video_height;
        vcfg.fps = video_fps;
        vcfg.output_format = livekit::VideoBufferType::NV12;

        if (dshow_cap->Init(vcfg, video_source)) {
            bool v_start = dshow_cap->Start();
            int actual_w = dshow_cap->GetNegotiatedWidth() > 0 ? dshow_cap->GetNegotiatedWidth() : init_w;
            int actual_h = dshow_cap->GetNegotiatedHeight() > 0 ? dshow_cap->GetNegotiatedHeight() : init_h;
            video_width = actual_w;
            video_height = actual_h;
            std::cout << "[VIDEO CAPTURE] DirectShow Camera capture started: "
                      << (v_start ? "SUCCESS" : "DEVICE BUSY / IN USE")
                      << " (Format: " << actual_w << "x" << actual_h << ")" << std::endl;
        } else {
            std::cout << "[VIDEO CAPTURE] Failed to initialize DirectShow capture!" << std::endl;
        }
    }

    std::atomic<bool> broadcaster_running{true};
    std::mutex telemetry_mutex;
    std::condition_variable telemetry_cv;

    // ------------------------------------------------------------------
    // Step 2: 监听 Ctrl+C 信号以优雅退出
    // ------------------------------------------------------------------
    asio::signal_set signals(io_ctx, SIGINT, SIGTERM);
    signals.async_wait([&](const std::error_code& error, int signal_number) {
        if (!error) {
            std::cout << "\n[SIGNAL] Received termination signal (" << signal_number << "), stopping broadcaster...\n";
            broadcaster_running.store(false);
            pattern_running.store(false);
            telemetry_cv.notify_all();
            if (wasapi_cap) wasapi_cap->Stop();
            if (dshow_cap) dshow_cap->Stop();
            room->Disconnect();
            io_ctx.stop();
        }
    });

    // ------------------------------------------------------------------
    // Step 3: 连接 LiveKit 房间并发布 Track
    // ------------------------------------------------------------------
    livekit::SignalOptions opts;
    opts.auto_subscribe = true;
    opts.connect_timeout = std::chrono::seconds(15);

    asio::co_spawn(io_ctx, [&]() -> asio::awaitable<void> {
        try {
            std::cout << "[STATUS] Connecting to LiveKit Server via WebSocket..." << std::endl;
            co_await room->ConnectAsync(url, token, opts);

            auto local = room->local_participant();
            if (local) {
                // 设置并测试 Participant Attributes (自定义属性下发)
                local->SetAttribute("broadcaster_version", "v2.0_cpp");
                local->SetAttribute("device_os", "Windows_Native");
                std::cout << "[ATTRIBUTES] Set Local Participant Attributes: broadcaster_version='v2.0_cpp', device_os='Windows_Native'" << std::endl;

                if (local_audio_track) {
                    co_await local->PublishTrackAsync(local_audio_track);
                    std::cout << "[PUBLISH] Published Local Audio Track (" << local_audio_track->name() << ")" << std::endl;
                }
                if (local_video_track) {
                    co_await local->PublishTrackAsync(local_video_track);
                    std::cout << "[PUBLISH] Published Local Video Track (" << local_video_track->name() << " - "
                              << video_width << "x" << video_height << ")" << std::endl;
                }
            }

            std::cout << "\n[BROADCASTING] Streaming live media to room! Press Ctrl+C to stop broadcasting.\n";
        } catch (const std::exception& e) {
            std::cout << "[EXCEPTION] Error in room connection coroutine: " << e.what() << std::endl;
            pattern_running.store(false);
            io_ctx.stop();
        }
    }, asio::detached);

    // 启动 3 分钟全量媒体遥测与质量监控线程
    std::thread telemetry_thread([&, room]() {
        uint64_t last_bytes_sent = 0;
        auto last_time = std::chrono::steady_clock::now();

        std::unique_lock<std::mutex> lock(telemetry_mutex);
        // 首次推流成功后等待 5 秒建立连接基线
        telemetry_cv.wait_for(lock, std::chrono::seconds(5), [&]() { return !broadcaster_running.load(); });

        while (broadcaster_running.load()) {
            try {
                livekit::RoomStatsReport stats = room->GetStatsSync();
                auto now = std::chrono::steady_clock::now();
                double elapsed_sec = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time).count() / 1000.0;
                if (elapsed_sec <= 0.01) elapsed_sec = 1.0;

                uint64_t current_bytes = stats.total_bytes_sent;
                double send_bitrate_mbps = 0.0;
                if (last_bytes_sent > 0 && current_bytes >= last_bytes_sent) {
                    send_bitrate_mbps = ((current_bytes - last_bytes_sent) * 8.0) / (elapsed_sec * 1000000.0);
                }
                last_bytes_sent = current_bytes;
                last_time = now;

                livekit::Telemetry::Instance().PrintMetricsReport(stats, send_bitrate_mbps);
            } catch (const std::exception& e) {
                std::cout << "[TELEMETRY ERROR] " << e.what() << std::endl;
            } catch (...) {}

            // 每 3 分钟自动刷新打印一次遥测面板
            telemetry_cv.wait_for(lock, std::chrono::minutes(3), [&]() { return !broadcaster_running.load(); });
        }
    });

    // 运行主事件循环
    io_ctx.run();

    broadcaster_running.store(false);
    pattern_running.store(false);
    if (telemetry_thread.joinable()) {
        telemetry_thread.join();
    }
    if (pattern_thread.joinable()) {
        pattern_thread.join();
    }
    if (wasapi_cap) wasapi_cap->Stop();
    if (dshow_cap) dshow_cap->Stop();

    std::cout << "[EXIT] Broadcaster terminated cleanly.\n";
    return 0;
}
