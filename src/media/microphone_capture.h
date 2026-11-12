#pragma once

#include "audio_capture_types.h"
#ifdef _WIN32
#include "wasapi_capture.h"
namespace livekit { using MicrophoneCapture = WasapiAudioCapture; }
#else
#include "audio_apm.h"
#include "../rtc/audio_source.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace livekit {
// Meeting microphone control for the native WebRTC ADM. PCM is sent by the
// native audio track, never injected into a second external AudioSource.
class MicrophoneCapture final {
public:
    static std::shared_ptr<MicrophoneCapture> Create();
    ~MicrophoneCapture();
    bool Init(const MicrophoneCaptureConfig&, std::shared_ptr<AudioSource>);
    bool Start();
    void Stop();
    bool IsRunning() const noexcept;
    void SetMute(bool muted) noexcept;
    bool IsMuted() const noexcept { return muted_.load(); }
    void EnableApm(const ApmConfig& = {}) {}
    void DisableApm() {}
    // The native source uses the engine's APM; never install a second APM as
    // the external capture/render reference processor.
    std::shared_ptr<AudioApmProcessor> apm_processor() const { return {}; }
    // Callbacks run on a native owner. Queue to the session/UI and never call
    // Start/Stop synchronously from a callback.
    void SetCaptureStateCallback(std::function<void(bool)> callback);
    void SetDeviceFrameCallback(std::function<void(std::uint64_t)> callback);
    std::uint64_t SwitchDeviceTracked(const std::string& device_id);
    std::uint64_t activeDeviceGeneration() const noexcept { return generation_.load(); }
private:
    void Notify(bool available);
    void ObserveDevice();
    std::mutex lifecycle_mutex_;
    mutable std::mutex operation_mutex_;
    std::mutex callback_mutex_;
    MicrophoneCaptureConfig config_;
    std::function<void(bool)> state_callback_;
    std::atomic<bool> prepared_{false}, muted_{true};
    std::atomic<bool> reported_available_{false};
    bool initialized_ = false;
    std::atomic<std::uint64_t> generation_{0};
    std::mutex observer_mutex_;
    std::condition_variable observer_wake_;
    std::atomic<bool> stop_observer_{false};
    std::thread observer_;
};
}
#endif
