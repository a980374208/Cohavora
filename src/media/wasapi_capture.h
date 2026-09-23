#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
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

    // 等待初次设备启动结果；无设备时返回 false，但保留热插拔监听。
    bool Start();

    // 停止音频捕获
    void Stop();

    // 是否已成功启动设备（后台监听线程存在不代表设备可用）。
    bool IsRunning() const noexcept { return is_running_.load(); }

    // 捕获线程通知实际设备状态；回调不得阻塞或直接调用 Start/Stop。
    // Qt 调用方须排队回到会话/UI owner，并校验会话是否仍有效。
    void SetCaptureStateCallback(std::function<void(bool)> callback);
    // Fires once for the first real 10 ms PCM frame produced by each selected
    // device generation. Synthetic loopback keepalive frames do not qualify.
    void SetDeviceFrameCallback(std::function<void(std::uint64_t)> callback);

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

    // 动态热切换输入音频设备 (空字符串表示使用系统默认麦克风)
    bool SwitchDevice(const std::string& device_id);
    std::uint64_t SwitchDeviceTracked(const std::string& device_id);
    std::uint64_t activeDeviceGeneration() const noexcept {
        return active_device_generation_.load(std::memory_order_acquire);
    }

private:
    bool InitializeAudioClient();
    void CleanupAudioClient();
    void CaptureThreadLoop();
    void SetCaptureRunning(bool running, bool force_notification = false);
    void NotifyFirstDeviceFrame();

    WasapiCaptureConfig config_;
    std::shared_ptr<AudioSource> audio_source_;

    std::atomic<bool> is_running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> is_muted_{false};
    std::atomic<bool> device_changed_{false};
    std::atomic<float> volume_{1.0f};

    std::thread capture_thread_;
    std::mutex lifecycle_mutex_;
    mutable std::mutex state_mutex_;
    std::mutex startup_mutex_;
    std::condition_variable startup_condition_;
    bool startup_complete_{false};
    std::mutex callback_mutex_;
    std::function<void(bool)> capture_state_callback_;
    std::function<void(std::uint64_t)> device_frame_callback_;
    std::atomic<std::uint64_t> requested_device_generation_{1};
    std::atomic<std::uint64_t> active_device_generation_{0};
    std::atomic<std::uint64_t> notified_device_generation_{0};

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
