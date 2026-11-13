#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <asio.hpp>
#include "api/peer_connection_interface.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "rtc_base/thread.h"
#include "audio_playout_device_selection.h"
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
#include "linux/linux_encoder_capability_types.h"
#endif

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
bool IsHardwareH264FormatSupported(const webrtc::SdpVideoFormat& format);
std::unique_ptr<webrtc::VideoEncoderFactory::EncoderSelectorInterface>
CreateAutoHardwareH264EncoderSelector(bool allow_vp8_fallback);
// Process-wide creation preference. Existing encoder sessions keep their backend.
// Accepted values: auto, software, amf, nvenc, qsv; unknown values select auto.
void SetVideoEncoderPreference(const std::string& preference);
std::string GetVideoEncoderPreference();

#if !defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
struct VideoEncoderBackendCapability {
    bool h264 = false;
    bool h265 = false;
};

struct VideoEncoderCapabilities {
    bool ready = false;
    VideoEncoderBackendCapability amf;
    VideoEncoderBackendCapability nvenc;
    VideoEncoderBackendCapability qsv;
};
#endif

// Reads the current snapshot without running or waiting for a hardware probe.
VideoEncoderCapabilities GetVideoEncoderCapabilities();
// Run from a background worker; vendor initialization may block. Windows uses
// the original once-only probe; Linux commits one snapshot per invalidation.
// Availability proves initialization/cleanup, not successful frame encoding.
void ProbeVideoEncoderCapabilities();
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
// Invalidation is cheap and never touches a driver; refresh runs on a worker.
// Retirement failure is process-sticky and cannot be cleared by either API.
void InvalidateVideoEncoderCapabilities(
    LinuxEncoderCapabilityReason reason = LinuxEncoderCapabilityReason::DeviceChanged);
// Immutable sender-owned policy. Empty approval always freezes software; a
// hardware policy requires the exact successful preflight snapshot/formats.
// Later settings or discovery changes never select a new backend here.
std::shared_ptr<webrtc::VideoEncoderFactory> CreateLinuxPublicationVideoEncoderFactory(
    const std::string& preference,
    const VideoEncoderCapabilities& capabilities,
    const std::vector<webrtc::SdpVideoFormat>& approved_hardware_h264_formats = {});
#endif

struct VideoEncoderPreflightLayer {
    int width = 0;
    int height = 0;
    int max_fps = 0;
    int max_bitrate_bps = 0;
};

struct VideoEncoderPreflightRequest {
    std::vector<VideoEncoderPreflightLayer> layers;
    std::vector<webrtc::SdpVideoFormat> formats;
    std::string preference;
    bool screenshare = false;
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
    VideoEncoderCapabilities linux_capabilities;
#endif
};

enum class VideoEncoderPreflightStatus {
    Ready, InvalidParameters, UnsupportedFormat, Unavailable,
    InitializationFailed, NotHardware, Cancelled, TimedOut, RetirementFailed
};

enum class VideoEncoderPreflightPhase { Startup, Ceiling };

struct VideoEncoderPreflightResult {
    VideoEncoderPreflightStatus status = VideoEncoderPreflightStatus::Unavailable;
    std::vector<webrtc::SdpVideoFormat> formats;
    std::string backend;
    std::size_t failed_layer = 0;
    int codec_status = 0;
    VideoEncoderPreflightPhase phase = VideoEncoderPreflightPhase::Startup;
    std::uint32_t tested_bitrate_bps = 0;
#if defined(COHAVORA_HAS_LINUX_HW_ENCODERS)
    VideoEncoderBackendCapability linux_capability;
#endif
    bool ok() const { return status == VideoEncoderPreflightStatus::Ready; }
};

// Call from a background executor. Success proves hardware initialization of
// every requested layer, including cleanup; it does not prove encoded output.
// Check independent groups at the startup floor and publication ceilings;
// sessions from the first group are retired before creating the second group.
// Auto checks only the first available backend in the runtime's cached order
// (Windows NVENC/QSV/AMF; Linux NVENC/QSV/VAAPI). Failure leaves codec fallback
// to the publication plan. Linux's complete group, cleanup and thread retirement
// must finish within the four-second budget; unresolved retirement fails closed.
VideoEncoderPreflightResult PreflightVideoEncoder(
    const VideoEncoderPreflightRequest& request,
    std::shared_ptr<std::atomic<bool>> cancelled = {});

namespace detail {
using VideoEncoderPreflightFactory = std::function<
    std::unique_ptr<webrtc::VideoEncoder>(const webrtc::SdpVideoFormat&)>;

// Synchronous core for an owned encoder sequence. All layers remain alive until
// the final initialization, then Release and destruction run on that sequence.
VideoEncoderPreflightResult CheckVideoEncoderPreflightFormat(
    const VideoEncoderPreflightRequest& request,
    const webrtc::SdpVideoFormat& format,
    const std::string& backend,
    const VideoEncoderPreflightFactory& factory,
    std::shared_ptr<std::atomic<bool>> cancelled = {});
} // namespace detail

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
