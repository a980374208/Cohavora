#include "wasapi_capture.h"
#include "media_converters.h"
#include "wasapi_enumerator.h"
#include <windows.h>
#include <avrt.h>
#include <wrl/implements.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <iostream>

namespace livekit {

// IMMNotificationClient 实现，用于捕获设备拔插与默认设备切换
class WasapiNotificationClient : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IMMNotificationClient>
{
public:
    explicit WasapiNotificationClient(std::weak_ptr<WasapiAudioCapture> capture)
        : capture_(capture) {}


    STDMETHODIMP OnDeviceStateChanged(LPCWSTR /*pwstrDeviceId*/, DWORD /*dwNewState*/) override {
        NotifyOwner();
        return S_OK;
    }

    STDMETHODIMP OnDeviceAdded(LPCWSTR /*pwstrDeviceId*/) override {
        NotifyOwner();
        return S_OK;
    }

    STDMETHODIMP OnDeviceRemoved(LPCWSTR /*pwstrDeviceId*/) override {
        NotifyOwner();
        return S_OK;
    }

    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow /*flow*/, ERole /*role*/, LPCWSTR /*pwstrDefaultDeviceId*/) override {
        NotifyOwner();
        return S_OK;
    }

    STDMETHODIMP OnPropertyValueChanged(LPCWSTR /*pwstrDeviceId*/, const PROPERTYKEY /*key*/) override {
        return S_OK;
    }

private:
    void NotifyOwner() {
        if (auto cap = capture_.lock()) {
            cap->OnDeviceChangedNotification();
        }
    }

    std::weak_ptr<WasapiAudioCapture> capture_;
    std::atomic<ULONG> ref_count_;
};

std::shared_ptr<WasapiAudioCapture> WasapiAudioCapture::Create() {
    return std::make_shared<WasapiAudioCapture>();
}

WasapiAudioCapture::WasapiAudioCapture() {
    audio_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    stop_event_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);
}

WasapiAudioCapture::~WasapiAudioCapture() {
    Stop();
    if (audio_event_) {
        CloseHandle(audio_event_);
        audio_event_ = nullptr;
    }
    if (stop_event_) {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
}

void WasapiAudioCapture::SetVolume(float volume) noexcept {
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 4.0f) volume = 4.0f;
    volume_.store(volume);
}

WasapiCaptureConfig WasapiAudioCapture::GetConfig() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(state_mutex_));
    return config_;
}

bool WasapiAudioCapture::Init(const WasapiCaptureConfig& config, std::shared_ptr<AudioSource> audio_source) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (is_running_.load()) {
        spdlog::warn("[WasapiAudioCapture] Cannot Init while running. Call Stop() first.");
        return false;
    }

    config_ = config;
    audio_source_ = audio_source;
    if (!apm_processor_) {
        apm_processor_ = AudioApmProcessor::Create();
    }
    return true;
}

bool WasapiAudioCapture::InitializeAudioClient() {
    CleanupAudioClient();

    if (!enumerator_) {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), &enumerator_);
        if (FAILED(hr) || !enumerator_) {
            spdlog::error("[WasapiAudioCapture] Failed to create MMDeviceEnumerator, hr=0x{:08x}", static_cast<uint32_t>(hr));
            return false;
        }

        // 注册全局热插拔监听 (即使暂无麦克风，也能监听后续插入事件)
        if (config_.auto_reconnect) {
            notify_client_ = Microsoft::WRL::Make<WasapiNotificationClient>(weak_from_this());
            enumerator_->RegisterEndpointNotificationCallback(notify_client_.Get());
        }
    }

    EDataFlow data_flow = (config_.type == WasapiCaptureType::Microphone) ? eCapture : eRender;
    HRESULT hr = S_OK;

    if (config_.device_id.empty()) {
        // 会议通话场景优先选择 eCommunications (插入耳机/耳麦时 Windows 默认分配给耳机麦克风)
        hr = enumerator_->GetDefaultAudioEndpoint(data_flow, eCommunications, &device_);
        if (FAILED(hr) || !device_) {
            hr = enumerator_->GetDefaultAudioEndpoint(data_flow, eConsole, &device_);
        }
        if (FAILED(hr) || !device_) {
            hr = enumerator_->GetDefaultAudioEndpoint(data_flow, eMultimedia, &device_);
        }
    } else {
        // 使用指定设备 ID
        int len = MultiByteToWideChar(CP_UTF8, 0, config_.device_id.c_str(), -1, nullptr, 0);
        std::wstring w_id(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, config_.device_id.c_str(), -1, &w_id[0], len);
        hr = enumerator_->GetDevice(w_id.c_str(), &device_);
    }

    if (FAILED(hr) || !device_) {
        spdlog::warn("[WasapiAudioCapture] Audio device endpoint not found (no active mic/headset currently). Waiting for device insertion...");
        return false;
    }

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audio_client_);
    if (FAILED(hr) || !audio_client_) {
        spdlog::error("[WasapiAudioCapture] Failed to activate IAudioClient, hr=0x{:08x}", static_cast<uint32_t>(hr));
        return false;
    }

    hr = audio_client_->GetMixFormat(&mix_format_);
    if (FAILED(hr) || !mix_format_) {
        spdlog::error("[WasapiAudioCapture] Failed to GetMixFormat, hr=0x{:08x}", static_cast<uint32_t>(hr));
        return false;
    }

    DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (config_.type == WasapiCaptureType::DesktopLoopback) {
        stream_flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    }

    // 默认请求 200ms 的环形缓冲区大小
    REFERENCE_TIME requested_duration = static_cast<REFERENCE_TIME>(config_.buffer_duration_ms) * 10000;
    if (requested_duration < 200000) requested_duration = 200000;

    hr = audio_client_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        stream_flags,
        requested_duration,
        0,
        mix_format_,
        nullptr
    );

    if (FAILED(hr)) {
        spdlog::error("[WasapiAudioCapture] IAudioClient::Initialize failed, hr=0x{:08x}", static_cast<uint32_t>(hr));
        return false;
    }

    hr = audio_client_->SetEventHandle(audio_event_);
    if (FAILED(hr)) {
        spdlog::error("[WasapiAudioCapture] SetEventHandle failed, hr=0x{:08x}", static_cast<uint32_t>(hr));
        return false;
    }

    hr = audio_client_->GetService(__uuidof(IAudioCaptureClient), &capture_client_);
    if (FAILED(hr) || !capture_client_) {
        spdlog::error("[WasapiAudioCapture] Failed to get IAudioCaptureClient, hr=0x{:08x}", static_cast<uint32_t>(hr));
        return false;
    }

    return true;
}

void WasapiAudioCapture::CleanupAudioClient() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (audio_client_) {
        audio_client_->Stop();
        audio_client_.Reset();
    }
    capture_client_.Reset();
    device_.Reset();

    if (mix_format_) {
        CoTaskMemFree(mix_format_);
        mix_format_ = nullptr;
    }
}

void WasapiAudioCapture::OnDeviceChangedNotification() {
    spdlog::info("[WasapiAudioCapture] Audio endpoint / device change event received (Headset/Mic plugged or default device changed).");
    device_changed_.store(true);
    if (audio_event_) {
        SetEvent(audio_event_);
    }
}

bool WasapiAudioCapture::SwitchDevice(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (config_.device_id == device_id) return true;
    config_.device_id = device_id;
    spdlog::info("[WasapiAudioCapture] Switching microphone device to ID: {}", device_id.empty() ? "(Default)" : device_id);
    OnDeviceChangedNotification();
    return true;
}

bool WasapiAudioCapture::Start() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (is_running_.load()) return true;

    ResetEvent(stop_event_);
    stop_requested_.store(false);

    capture_thread_ = std::thread(&WasapiAudioCapture::CaptureThreadLoop, this);
    is_running_.store(true);
    return true;
}

void WasapiAudioCapture::Stop() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        stop_requested_.store(true);
        if (stop_event_) {
            SetEvent(stop_event_);
        }
        if (enumerator_ && notify_client_) {
            enumerator_->UnregisterEndpointNotificationCallback(notify_client_.Get());
            notify_client_.Reset();
        }
        enumerator_.Reset();
    }

    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    is_running_.store(false);
}

void WasapiAudioCapture::CaptureThreadLoop() {
    HRESULT hr_co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // MMCSS 注册高优先级专业音频捕获调度
    DWORD task_index = 0;
    HANDLE mmcss_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    if (!mmcss_handle) {
        spdlog::debug("[WasapiAudioCapture] AvSetMmThreadCharacteristicsW Pro Audio fallback to Audio");
        mmcss_handle = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
    }

    // 尝试初次初始化 (若当前未插麦克风也不会退出线程，而是保持后台监听等待插入)
    if (InitializeAudioClient()) {
        audio_client_->Start();
    } else {
        spdlog::info("[WasapiAudioCapture] No audio device on startup. Background watcher is listening for hotplug...");
    }

    HANDLE wait_handles[2] = { stop_event_, audio_event_ };
    auto last_packet_time = std::chrono::steady_clock::now();
    std::vector<int16_t> fifo_buffer;
    const int frames_per_10ms = config_.target_sample_rate / 100;
    const size_t samples_per_10ms = static_cast<size_t>(frames_per_10ms * config_.target_channels);

    while (!stop_requested_.load()) {
        DWORD wait_res = WaitForMultipleObjects(2, wait_handles, FALSE, 20); // 20ms timeout

        if (wait_res == WAIT_OBJECT_0) {
            // Stop requested
            break;
        }

        if (device_changed_.exchange(false)) {
            spdlog::info("[WasapiAudioCapture] Device changed event triggered. Re-initializing audio client to new default device...");
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            if (InitializeAudioClient()) {
                if (SUCCEEDED(audio_client_->Start())) {
                    spdlog::info("[WasapiAudioCapture] Audio client successfully restarted on newly plugged/selected device!");
                }
            }
            continue;
        }

        if (!capture_client_ || !audio_client_) {
            continue;
        }

        UINT32 next_packet_size = 0;
        HRESULT hr = capture_client_->GetNextPacketSize(&next_packet_size);

        if (FAILED(hr)) {
            // 设备可能断开或需要重置
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_RESOURCES_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
                spdlog::warn("[WasapiAudioCapture] Audio device invalidated (hr=0x{:08x}). Attempting to re-initialize...", static_cast<uint32_t>(hr));
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (InitializeAudioClient()) {
                    audio_client_->Start();
                }
            }
            continue;
        }

        bool captured_any = false;
        while (next_packet_size > 0 && !stop_requested_.load()) {
            BYTE* pData = nullptr;
            UINT32 numFramesAvailable = 0;
            DWORD flags = 0;
            UINT64 devPos = 0;
            UINT64 qpcPos = 0;

            hr = capture_client_->GetBuffer(&pData, &numFramesAvailable, &flags, &devPos, &qpcPos);
            if (SUCCEEDED(hr)) {
                captured_any = true;
                last_packet_time = std::chrono::steady_clock::now();

                bool is_silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) || is_muted_.load();
                float current_vol = volume_.load();

                std::vector<int16_t> pcm_out;
                if (!is_silent && pData) {
                    MediaConverters::ProcessWasapiAudioBuffer(
                        pData,
                        numFramesAvailable,
                        mix_format_,
                        config_.target_sample_rate,
                        config_.target_channels,
                        pcm_out
                    );

                    // 应用软件音量调节
                    if (std::abs(current_vol - 1.0f) > 0.01f) {
                        for (auto& sample : pcm_out) {
                            float scaled = sample * current_vol;
                            if (scaled > 32767.0f) scaled = 32767.0f;
                            else if (scaled < -32768.0f) scaled = -32768.0f;
                            sample = static_cast<int16_t>(scaled);
                        }
                    }
                } else {
                    // 静音模式下填充 0
                    uint32_t target_frames = static_cast<uint32_t>(
                        static_cast<uint64_t>(numFramesAvailable) * config_.target_sample_rate / mix_format_->nSamplesPerSec);
                    if (target_frames == 0) target_frames = 1;
                    pcm_out.assign(target_frames * config_.target_channels, 0);
                }

                if (!pcm_out.empty()) {
                    fifo_buffer.insert(fifo_buffer.end(), pcm_out.begin(), pcm_out.end());
                }

                capture_client_->ReleaseBuffer(numFramesAvailable);
            }

            hr = capture_client_->GetNextPacketSize(&next_packet_size);
            if (FAILED(hr)) break;
        }

        // 统一按 10ms 切片投递给 APM 及 WebRTC AudioSource (WebRTC/APM 严格要求 10ms 帧长)
        while (fifo_buffer.size() >= samples_per_10ms && audio_source_) {
            std::vector<int16_t> chunk(fifo_buffer.begin(), fifo_buffer.begin() + samples_per_10ms);
            fifo_buffer.erase(fifo_buffer.begin(), fifo_buffer.begin() + samples_per_10ms);

            AudioFrame frame(std::move(chunk), config_.target_sample_rate, config_.target_channels, frames_per_10ms);
            if (apm_processor_) {
                frame = apm_processor_->ProcessCaptureFrame(frame);
            }
            audio_source_->captureFrame(frame);
        }

        // 麦克风捕获守卫：若超过 1 秒完全未收到任何音频包（常见于插拔耳机导致驱动静默断流），自动热重连
        if (!captured_any && config_.type == WasapiCaptureType::Microphone && config_.auto_reconnect) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_packet_time).count();
            if (elapsed >= 1000) {
                last_packet_time = now;
                spdlog::warn("[WasapiAudioCapture] Microphone capture stalled (1s no data). Re-initializing device...");
                std::cout << "[WASAPI WATCHDOG] Microphone stream stalled. Re-initializing default microphone..." << std::endl;
                if (InitializeAudioClient()) {
                    if (SUCCEEDED(audio_client_->Start())) {
                        std::cout << "[WASAPI WATCHDOG] Successfully recovered microphone stream!" << std::endl;
                    }
                }
            }
        }

        // Loopback 模式下，当系统无任何应用播放声音时，WASAPI 不会产生音频包。
        // 参照 OBS 行为：若超过 40ms 无数据，自动注入 20ms 的静音帧以维持 WebRTC 时钟同步。
        if (!captured_any && config_.type == WasapiCaptureType::DesktopLoopback) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_packet_time).count();
            if (elapsed >= 40) {
                last_packet_time = now;
                std::vector<int16_t> silent_pcm(samples_per_10ms * 2, 0);
                fifo_buffer.insert(fifo_buffer.end(), silent_pcm.begin(), silent_pcm.end());
                while (fifo_buffer.size() >= samples_per_10ms && audio_source_) {
                    std::vector<int16_t> chunk(fifo_buffer.begin(), fifo_buffer.begin() + samples_per_10ms);
                    fifo_buffer.erase(fifo_buffer.begin(), fifo_buffer.begin() + samples_per_10ms);

                    AudioFrame silent_frame(std::move(chunk), config_.target_sample_rate, config_.target_channels, frames_per_10ms);
                    audio_source_->captureFrame(silent_frame);
                }
            }
        }
    }

    CleanupAudioClient();

    if (mmcss_handle) {
        AvRevertMmThreadCharacteristics(mmcss_handle);
    }
    if (SUCCEEDED(hr_co)) {
        CoUninitialize();
    }
}

} // namespace livekit
