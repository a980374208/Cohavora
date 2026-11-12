#include "microphone_capture.h"
#include "../rtc/webrtc_manager.h"

namespace livekit {
namespace {
// The process has one native ADM. A retired capture cannot stop or switch a
// successor's microphone while asynchronous session cleanup is draining.
std::mutex native_owner_mutex;
const MicrophoneCapture* native_owner = nullptr;
}
std::shared_ptr<MicrophoneCapture> MicrophoneCapture::Create() {
    return std::make_shared<MicrophoneCapture>();
}
MicrophoneCapture::~MicrophoneCapture() { Stop(); }
bool MicrophoneCapture::Init(const MicrophoneCaptureConfig& config, std::shared_ptr<AudioSource>) {
    std::lock_guard lock(operation_mutex_);
    if (prepared_ || config.type != AudioCaptureType::Microphone) return false;
    config_ = config;
    initialized_ = true;
    return true;
}
bool MicrophoneCapture::Start() {
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    bool success = false;
    {
        std::lock_guard owner_lock(native_owner_mutex);
        std::lock_guard lock(operation_mutex_);
        if (!initialized_ || (native_owner && native_owner != this)) return false;
        native_owner = this;
        auto& manager = WebRTCManager::Instance();
        success = manager.Initialize() && manager.SetRecordingDeviceById(config_.device_id) && manager.EnsureRecording();
        // Probe actual acquisition during preparation, then honour muted entry.
        if (muted_.load() || !success) success = manager.StopRecording() && success;
        prepared_.store(success);
        if (success) ++generation_;
        if (!success) native_owner = nullptr;
        Notify(success);
    }
    if (!observer_.joinable()) {
        stop_observer_.store(false);
        observer_ = std::thread([this] { ObserveDevice(); });
    }
    return success;
}
void MicrophoneCapture::Stop() {
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    stop_observer_.store(true);
    observer_wake_.notify_all();
    if (observer_.joinable()) observer_.join();
    {
        std::lock_guard owner_lock(native_owner_mutex);
        std::lock_guard lock(operation_mutex_);
        prepared_.store(false);
        initialized_ = false;
        muted_.store(true);
        if (native_owner == this) {
            WebRTCManager::Instance().StopRecording();
            native_owner = nullptr;
        }
        Notify(false);
    }
}
bool MicrophoneCapture::IsRunning() const noexcept {
    // Qt reads only the last verified availability. Native validation and
    // default-device refresh belong to ObserveDevice/control workers.
    return prepared_.load(std::memory_order_acquire);
}
void MicrophoneCapture::SetMute(bool muted) noexcept {
    bool success = true;
    {
        std::lock_guard owner_lock(native_owner_mutex);
        std::lock_guard lock(operation_mutex_);
        muted_.store(muted);
        if (!initialized_ || (native_owner && native_owner != this)) return;
        if (muted && native_owner != this) return;
        native_owner = this;
        auto& manager = WebRTCManager::Instance();
        success = muted ? manager.StopRecording() :
            manager.Initialize() && manager.SetRecordingDeviceById(config_.device_id) && manager.EnsureRecording();
        if (!muted || !success) prepared_.store(success);
        // Stopping a failed device does not make it available. Notifications
        // stay ordered with native state transitions under the owner lock.
        Notify(success && prepared_.load());
    }
}
std::uint64_t MicrophoneCapture::SwitchDeviceTracked(const std::string& device_id) {
    bool success;
    std::uint64_t result = 0;
    {
        std::lock_guard owner_lock(native_owner_mutex);
        std::lock_guard lock(operation_mutex_);
        if (!initialized_ || (native_owner && native_owner != this)) return 0;
        native_owner = this;
        auto& manager = WebRTCManager::Instance();
        success = manager.Initialize() && manager.SetRecordingDeviceById(device_id);
        if (success) success = manager.EnsureRecording();
        if (muted_.load()) success = manager.StopRecording() && success;
        if (success) {
            config_.device_id = device_id;
            prepared_.store(true);
            result = ++generation_;
        }
        const bool available = prepared_.load() &&
            (muted_.load() || manager.RecordingActive());
        if (!available) prepared_.store(false);
        Notify(available);
    }
    return result;
}
void MicrophoneCapture::SetCaptureStateCallback(std::function<void(bool)> callback) {
    std::lock_guard lock(callback_mutex_);
    state_callback_ = std::move(callback);
}
void MicrophoneCapture::SetDeviceFrameCallback(std::function<void(std::uint64_t)>) {
    // ADM does not expose a per-device first-PCM callback. Completion is checked
    // synchronously by the shared UI; do not report a fabricated frame event.
}
void MicrophoneCapture::Notify(bool available) {
    if (reported_available_.exchange(available) == available) return;
    std::function<void(bool)> callback;
    { std::lock_guard lock(callback_mutex_); callback = state_callback_; }
    if (callback) callback(available);
}
void MicrophoneCapture::ObserveDevice() {
    std::unique_lock wait_lock(observer_mutex_);
    while (!observer_wake_.wait_for(wait_lock, std::chrono::milliseconds(500),
                                   [this] { return stop_observer_.load(); })) {
        wait_lock.unlock();
        {
            std::lock_guard owner_lock(native_owner_mutex);
            std::lock_guard operation_lock(operation_mutex_);
            const bool available = native_owner == this && prepared_.load() &&
                (muted_.load() || WebRTCManager::Instance().RecordingActive());
            // Remember real acquisition failure across the UI's automatic mute.
            // A successful StopRecording must never manufacture device recovery.
            if (!available) prepared_.store(false);
            if (!stop_observer_.load()) Notify(available);
        }
        wait_lock.lock();
    }
}
}
