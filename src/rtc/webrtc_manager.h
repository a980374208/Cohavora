#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <asio.hpp>
#include "api/peer_connection_interface.h"
#include "rtc_base/thread.h"

namespace livekit {

class AudioApmProcessor;
class ExecutorCallbackGate;

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
    // Empty ID restores the Windows default playback endpoint. Explicit IDs
    // are matched against the ADM's endpoint GUIDs on its owning worker thread.
    bool SetPlayoutDeviceById(const std::string& device_id);
    // Success means that the output stream actually started, not just selected.
    bool EnsurePlayout();

    // 跨线程安全 SDP 协商辅助函数
    void CreateOffer(
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
        asio::any_io_executor executor,
        std::function<void(const std::string& sdp, const std::string& error)> callback,
        bool ice_restart = false,
        std::shared_ptr<ExecutorCallbackGate> callback_gate = {});

    void CreateAnswer(
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
        asio::any_io_executor executor,
        std::function<void(const std::string& sdp, const std::string& error)> callback,
        std::shared_ptr<ExecutorCallbackGate> callback_gate = {});

    void SetRemoteDescription(
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
        const std::string& type,
        const std::string& sdp,
        asio::any_io_executor executor,
        std::function<void(const std::string& error)> callback,
        std::shared_ptr<ExecutorCallbackGate> callback_gate = {});

    void SetLocalDescription(
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
        const std::string& type,
        const std::string& sdp,
        asio::any_io_executor executor,
        std::function<void(const std::string& error)> callback,
        std::shared_ptr<ExecutorCallbackGate> callback_gate = {});

private:
    WebRTCManager() = default;
    ~WebRTCManager();

private:
    std::mutex mutex_;
    bool initialized_ = false;

    std::unique_ptr<webrtc::Thread> network_thread_;
    std::unique_ptr<webrtc::Thread> worker_thread_;
    std::unique_ptr<webrtc::Thread> signaling_thread_;

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm_;

    mutable std::mutex apm_mutex_;
    std::shared_ptr<AudioApmProcessor> apm_processor_;
};

} // namespace livekit
