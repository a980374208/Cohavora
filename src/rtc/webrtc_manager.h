#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <asio.hpp>
#include "api/peer_connection_interface.h"
#include "rtc_base/thread.h"
#include "audio_playout_device_selection.h"

namespace webrtc {
class VideoDecoderFactory;
class VideoEncoderFactory;
}

namespace livekit {

namespace diagnostic { class SdpNegotiationTrace; }

class AudioApmProcessor;
class ExecutorCallbackGate;

struct AudioDeviceInfo {
    std::string id;
    std::string name;
};

#if !defined(_WIN32)
namespace detail {

// VoiceEngine starts ADM recording when it adds a send stream. Native capture
// must wait until the application explicitly selects the microphone path.
class NativeRecordingGate {
public:
    template <typename Device>
    int32_t InitRecording(Device& device) {
        return permitted_ ? device.InitRecording() : 0;
    }

    template <typename Device>
    int32_t StartRecording(Device& device) {
        return permitted_ ? device.StartRecording() : 0;
    }

    template <typename Device>
    bool StartExplicitly(Device& device) {
        permitted_ = true;
        if (device.Recording()) return true;
        if ((!device.RecordingIsInitialized() && device.InitRecording() != 0) ||
            device.StartRecording() != 0 || !device.Recording()) {
            permitted_ = false;
            device.StopRecording();
            return false;
        }
        return true;
    }

    template <typename Device>
    bool StopExplicitly(Device& device) {
        permitted_ = false;
        if (!device.Recording() && !device.RecordingIsInitialized()) return true;
        return device.StopRecording() == 0 && !device.Recording();
    }

    void Revoke() { permitted_ = false; }

private:
    bool permitted_ = false;
};

} // namespace detail
#endif

std::unique_ptr<webrtc::VideoEncoderFactory> CreateVideoEncoderFactory();
std::unique_ptr<webrtc::VideoDecoderFactory> CreateVideoDecoderFactory();
bool IsVideoEncoderFormatSupported(
    const std::string& codec_name,
    const std::string& scalability_mode = {});

class WebRTCManager {
public:
    static WebRTCManager& Instance();

    WebRTCManager(const WebRTCManager&) = delete;
    WebRTCManager& operator=(const WebRTCManager&) = delete;

    bool Initialize();
    void Deinitialize();

    webrtc::Thread* network_thread() const { return network_thread_.get(); }
    webrtc::Thread* worker_thread() const { return worker_thread_.get(); }
    webrtc::Thread* signaling_thread() const { return signaling_thread_.get(); }

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory() const { return factory_; }
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm() const { return adm_; }

    // APM 3A 扬声器下行渲染参考流对接
    void SetApmProcessor(std::shared_ptr<AudioApmProcessor> processor);
    std::shared_ptr<AudioApmProcessor> apm_processor() const;
    void ResetApmProcessor();

    // The numeric overload accepts an ADM index, never a WASAPI enumeration index.
    bool SetPlayoutDevice(uint16_t index);
    // Empty ID follows the platform default. Windows IDs are ADM endpoint
    // GUIDs; Linux nonempty IDs bind an index to its current name.
    bool SetPlayoutDeviceById(const std::string& device_id);
    // Reports the endpoint retained after a failed switch and whether the
    // native output stream is actually playing.
    PlayoutSwitchResult SwitchPlayoutDeviceById(const std::string& device_id);
    std::vector<AudioDeviceInfo> ListRecordingDevices();
    std::vector<AudioDeviceInfo> ListPlayoutDevices();
    // Empty ID follows the Linux system default. On Windows native recording
    // remains disabled in favour of the existing WASAPI capture path.
    bool SetRecordingDeviceById(const std::string& device_id);
    bool EnsureRecording();
    // Read native ADM state after a failed switch or publication change.
    bool RecordingActive();
    // Idempotent after leave or before ADM initialization.
    bool StopRecording();
    // Success means that the output stream actually started, not just selected.
    // Reject startup until a PeerConnection has registered the audio callback.
    bool EnsurePlayout();

    // 跨线程安全 SDP 协商辅助函数
    void CreateOffer(
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
        asio::any_io_executor executor,
        std::function<void(const std::string& sdp, const std::string& error)> callback,
        bool ice_restart = false,
        std::shared_ptr<ExecutorCallbackGate> callback_gate = {},
        std::shared_ptr<diagnostic::SdpNegotiationTrace> trace = {});

    void CreateAnswer(
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
        asio::any_io_executor executor,
        std::function<void(const std::string& sdp, const std::string& error)> callback,
        std::shared_ptr<ExecutorCallbackGate> callback_gate = {},
        std::shared_ptr<diagnostic::SdpNegotiationTrace> trace = {});

    void SetRemoteDescription(
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
        const std::string& type,
        const std::string& sdp,
        asio::any_io_executor executor,
        std::function<void(const std::string& error)> callback,
        std::shared_ptr<ExecutorCallbackGate> callback_gate = {},
        std::shared_ptr<diagnostic::SdpNegotiationTrace> trace = {});

    void SetLocalDescription(
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
        const std::string& type,
        const std::string& sdp,
        asio::any_io_executor executor,
        std::function<void(const std::string& error)> callback,
        std::shared_ptr<ExecutorCallbackGate> callback_gate = {},
        std::shared_ptr<diagnostic::SdpNegotiationTrace> trace = {});

private:
    WebRTCManager() = default;
    ~WebRTCManager();
    void DeinitializeLocked();
#if !defined(_WIN32)
    // Called only on the ADM worker while mutex_ is held.
    bool RefreshDefaultPlayoutOnWorker();
    bool RefreshDefaultRecordingOnWorker();
#endif

private:
    std::mutex mutex_;
    bool initialized_ = false;
    bool ssl_initialized_ = false;

    std::unique_ptr<webrtc::Thread> network_thread_;
    std::unique_ptr<webrtc::Thread> worker_thread_;
    std::unique_ptr<webrtc::Thread> signaling_thread_;

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm_;
    uint16_t playout_device_index_ = 0;
    bool playout_device_selected_ = false;
    uint16_t recording_device_index_ = 0;
    bool recording_device_selected_ = false;
#if !defined(_WIN32)
    std::optional<std::string> playout_device_identity_;
    std::optional<std::string> recording_device_identity_;
    bool playout_device_is_default_ = true;
    bool recording_device_is_default_ = true;
#endif

    mutable std::mutex apm_mutex_;
    std::shared_ptr<AudioApmProcessor> apm_processor_;
};

} // namespace livekit
