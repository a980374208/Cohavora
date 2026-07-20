#include "webrtc_manager.h"
#include <asio.hpp>
#include "rtc_base/ssl_adapter.h"
#include "api/create_peerconnection_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "api/video_codecs/video_decoder_factory.h"
#include <iostream>

namespace livekit {

namespace {

class EmptyVideoEncoderFactory : public webrtc::VideoEncoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return {};
    }
    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment&, const webrtc::SdpVideoFormat&) override {
        return nullptr;
    }
};

class EmptyVideoDecoderFactory : public webrtc::VideoDecoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return {};
    }
    std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment&, const webrtc::SdpVideoFormat&) override {
        return nullptr;
    }
};

} // namespace

WebRTCManager& WebRTCManager::Instance() {
    static WebRTCManager instance;
    return instance;
}

WebRTCManager::~WebRTCManager() {
    Deinitialize();
}

bool WebRTCManager::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return true;
    }

    std::cout << "WebRTCManager: Initializing SSL and starting threads..." << std::endl;
    if (!webrtc::InitializeSSL()) {
        std::cerr << "WebRTCManager: Failed to initialize SSL" << std::endl;
        return false;
    }

    network_thread_ = webrtc::Thread::CreateWithSocketServer();
    worker_thread_ = webrtc::Thread::Create();
    signaling_thread_ = webrtc::Thread::Create();

    if (!network_thread_->Start() || !worker_thread_->Start() || !signaling_thread_->Start()) {
        std::cerr << "WebRTCManager: Failed to start WebRTC helper threads" << std::endl;
        webrtc::CleanupSSL();
        return false;
    }

    std::cout << "WebRTCManager: Creating PeerConnectionFactory..." << std::endl;
    
    auto audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
    auto audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
    auto video_encoder_factory = std::make_unique<EmptyVideoEncoderFactory>();
    auto video_decoder_factory = std::make_unique<EmptyVideoDecoderFactory>();

    factory_ = webrtc::CreatePeerConnectionFactory(
        network_thread_.get(),
        worker_thread_.get(),
        signaling_thread_.get(),
        webrtc::scoped_refptr<webrtc::AudioDeviceModule>(), 
        audio_encoder_factory,
        audio_decoder_factory,
        std::move(video_encoder_factory),
        std::move(video_decoder_factory),
        webrtc::scoped_refptr<webrtc::AudioMixer>(), 
        webrtc::scoped_refptr<webrtc::AudioProcessing>()  
    );

    if (!factory_) {
        std::cerr << "WebRTCManager: Failed to create PeerConnectionFactory" << std::endl;
        Deinitialize();
        return false;
    }

    initialized_ = true;
    std::cout << "WebRTCManager: Initialized successfully!" << std::endl;
    return true;
}

void WebRTCManager::Deinitialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return;
    }

    std::cout << "WebRTCManager: Deinitializing factory and threads..." << std::endl;

    // Release the factory. This triggers internal WebRTC cleanup:
    // WebRtcVoiceEngine and AudioDeviceWindowsCore destruction work is
    // *dispatched* (asynchronously posted) to the worker thread's message queue.
    factory_ = nullptr;

    // Flush each WebRTC thread's message queue with a blocking no-op BEFORE
    // calling Stop(). This guarantees all factory destructor side-effects
    // (e.g. AudioDeviceWindowsCore::~AudioDeviceWindowsCore via
    // WebRtcVoiceEngine destructor dispatch) complete while threads are still
    // alive, preventing the 0xC0000005 access violation on COM vtable teardown.
    if (worker_thread_) {
        worker_thread_->BlockingCall([]() {});
    }
    if (signaling_thread_) {
        signaling_thread_->BlockingCall([]() {});
    }
    if (network_thread_) {
        network_thread_->BlockingCall([]() {});
    }

    if (signaling_thread_) {
        signaling_thread_->Stop();
        signaling_thread_.reset();
    }
    if (worker_thread_) {
        worker_thread_->Stop();
        worker_thread_.reset();
    }
    if (network_thread_) {
        network_thread_->Stop();
        network_thread_.reset();
    }

    webrtc::CleanupSSL();
    initialized_ = false;
    std::cout << "WebRTCManager: Deinitialized successfully." << std::endl;
}

// SDP 观察者桥接类实现
class CreateSdpObserverProxy : public webrtc::CreateSessionDescriptionObserver {
public:
    static webrtc::scoped_refptr<CreateSdpObserverProxy> Create(
        asio::any_io_executor executor,
        std::function<void(const std::string& sdp, const std::string& error)> callback) {
        return webrtc::make_ref_counted<CreateSdpObserverProxy>(executor, callback);
    }

    CreateSdpObserverProxy(
        asio::any_io_executor executor,
        std::function<void(const std::string& sdp, const std::string& error)> callback)
        : executor_(executor), callback_(callback) {}

    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
        std::string sdp;
        desc->ToString(&sdp);
        asio::post(executor_, [callback = callback_, sdp]() {
            callback(sdp, "");
        });
    }

    void OnFailure(webrtc::RTCError error) override {
        std::string err_msg = error.message();
        asio::post(executor_, [callback = callback_, err_msg]() {
            callback("", err_msg);
        });
    }

private:
    asio::any_io_executor executor_;
    std::function<void(const std::string& sdp, const std::string& error)> callback_;
};

class SetSdpObserverProxy : public webrtc::SetSessionDescriptionObserver {
public:
    static webrtc::scoped_refptr<SetSdpObserverProxy> Create(
        asio::any_io_executor executor,
        std::function<void(const std::string& error)> callback) {
        return webrtc::make_ref_counted<SetSdpObserverProxy>(executor, callback);
    }

    SetSdpObserverProxy(
        asio::any_io_executor executor,
        std::function<void(const std::string& error)> callback)
        : executor_(executor), callback_(callback) {}

    void OnSuccess() override {
        asio::post(executor_, [callback = callback_]() {
            callback("");
        });
    }

    void OnFailure(webrtc::RTCError error) override {
        std::string err_msg = error.message();
        asio::post(executor_, [callback = callback_, err_msg]() {
            callback(err_msg);
        });
    }

private:
    asio::any_io_executor executor_;
    std::function<void(const std::string& error)> callback_;
};

void WebRTCManager::CreateOffer(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
    asio::any_io_executor executor,
    std::function<void(const std::string& sdp, const std::string& error)> callback) {
    
    signaling_thread_->PostTask([pc, executor, callback]() {
        auto observer = CreateSdpObserverProxy::Create(executor, callback);
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        pc->CreateOffer(observer.get(), options);
    });
}

void WebRTCManager::SetRemoteDescription(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
    const std::string& type,
    const std::string& sdp,
    asio::any_io_executor executor,
    std::function<void(const std::string& error)> callback) {
    
    signaling_thread_->PostTask([pc, type, sdp, executor, callback]() {
        webrtc::SdpParseError err;
        webrtc::SdpType sdp_type = (type == "answer") ? webrtc::SdpType::kAnswer : webrtc::SdpType::kOffer;
        std::unique_ptr<webrtc::SessionDescriptionInterface> session_desc =
            webrtc::CreateSessionDescription(sdp_type, sdp, &err);
        
        if (!session_desc) {
            std::string err_msg = err.description;
            asio::post(executor, [callback, err_msg]() {
                callback(err_msg);
            });
            return;
        }

        auto observer = SetSdpObserverProxy::Create(executor, callback);
        pc->SetRemoteDescription(observer.get(), session_desc.release());
    });
}

void WebRTCManager::SetLocalDescription(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
    const std::string& type,
    const std::string& sdp,
    asio::any_io_executor executor,
    std::function<void(const std::string& error)> callback) {
    
    signaling_thread_->PostTask([pc, type, sdp, executor, callback]() {
        webrtc::SdpParseError err;
        webrtc::SdpType sdp_type = (type == "answer") ? webrtc::SdpType::kAnswer : webrtc::SdpType::kOffer;
        std::unique_ptr<webrtc::SessionDescriptionInterface> session_desc =
            webrtc::CreateSessionDescription(sdp_type, sdp, &err);
        
        if (!session_desc) {
            std::string err_msg = err.description;
            asio::post(executor, [callback, err_msg]() {
                callback(err_msg);
            });
            return;
        }

        auto observer = SetSdpObserverProxy::Create(executor, callback);
        pc->SetLocalDescription(observer.get(), session_desc.release());
    });
}

} // namespace livekit
