#pragma once

#include <memory>
#include <mutex>
#include <asio.hpp>
#include "api/peer_connection_interface.h"
#include "rtc_base/thread.h"

namespace livekit {

class WebRTCManager {
public:
    static WebRTCManager& Instance();

    WebRTCManager(const WebRTCManager&) = delete;
    WebRTCManager& operator=(const WebRTCManager&) = delete;

    bool Initialize();
    void Deinitialize();

    webrtc::Thread* network_thread() const { return network_thread_.get(); }
    webrtc::Thread* worker_thread() const { return signaling_thread_.get(); }
    webrtc::Thread* signaling_thread() const { return signaling_thread_.get(); }

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory() const { return factory_; }

    // 跨线程安全 SDP 协商辅助函数
    void CreateOffer(
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
        asio::any_io_executor executor,
        std::function<void(const std::string& sdp, const std::string& error)> callback);

    void CreateAnswer(
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
        asio::any_io_executor executor,
        std::function<void(const std::string& sdp, const std::string& error)> callback);

    void SetRemoteDescription(
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
        const std::string& type,
        const std::string& sdp,
        asio::any_io_executor executor,
        std::function<void(const std::string& error)> callback);

    void SetLocalDescription(
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
        const std::string& type,
        const std::string& sdp,
        asio::any_io_executor executor,
        std::function<void(const std::string& error)> callback);

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
};

} // namespace livekit
