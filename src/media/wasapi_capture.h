#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include "wasapi_types.h"
#include "audio_source.h"
#include "audio_apm.h"

namespace livekit {

class WasapiNotificationClient;

class WasapiAudioCapture : public std::enable_shared_from_this<WasapiAudioCapture> {
public:
    static std::shared_ptr<WasapiAudioCapture> Create();

    WasapiAudioCapture();
    ~WasapiAudioCapture();

    WasapiAudioCapture(const WasapiAudioCapture&) = delete;
    WasapiAudioCapture& operator=(const WasapiAudioCapture&) = delete;

    // 初始化捕获器
    bool Init(const WasapiCaptureConfig& config, std::shared_ptr<AudioSource> audio_source);

    // 启动音频捕获线程
    bool Start();

    // 停止音频捕获
    void Stop();

    // 是否正在运行
    bool IsRunning() const noexcept { return is_running_.load(); }

    // 静音与音量调节
    void SetMute(bool mute) noexcept { is_muted_.store(mute); }
    bool IsMuted() const noexcept { return is_muted_.load(); }

    void SetVolume(float volume) noexcept;
    float GetVolume() const noexcept { return volume_.load(); }

    // APM 3A (AEC/ANS/AGC) 音频处理控制
    void EnableApm(const ApmConfig& config = {}) { apm_processor_ = AudioApmProcessor::Create(config); }
    void DisableApm() { apm_processor_ = nullptr; }
    std::shared_ptr<AudioApmProcessor> apm_processor() const { return apm_processor_; }

    // 获取当前捕获配置
    WasapiCaptureConfig GetConfig() const;

    // 设备拔插或默认设备切换时的重启通知接口
    void OnDeviceChangedNotification();

private:
    bool InitializeAudioClient();
    void CleanupAudioClient();
    void CaptureThreadLoop();

    WasapiCaptureConfig config_;
    std::shared_ptr<AudioSource> audio_source_;

    std::atomic<bool> is_running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> is_muted_{false};
    std::atomic<float> volume_{1.0f};

    std::thread capture_thread_;
    std::mutex state_mutex_;

    // Windows Core Audio COM 接口
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<IMMDevice> device_;
    Microsoft::WRL::ComPtr<IAudioClient> audio_client_;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_client_;
    WAVEFORMATEX* mix_format_{nullptr};

    HANDLE audio_event_{nullptr};
    HANDLE stop_event_{nullptr};

    // 热插拔监听
    Microsoft::WRL::ComPtr<WasapiNotificationClient> notify_client_;

    // WebRTC APM 3A 处理模块
    std::shared_ptr<AudioApmProcessor> apm_processor_;
};

} // namespace livekit
