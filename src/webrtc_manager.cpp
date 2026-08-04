#include "webrtc_manager.h"
#include <asio.hpp>
#include "rtc_base/ssl_adapter.h"
#include "api/create_peerconnection_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/sequence_checker.h"
#include <iostream>


namespace webrtc {
namespace webrtc_checks_impl {
    void FatalLog(char const* file, int line) {}
}
}

namespace livekit {

namespace {

class CustomVideoEncoderFactory : public webrtc::VideoEncoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return {
            webrtc::SdpVideoFormat("VP8"),
            webrtc::SdpVideoFormat("H264")
        };
    }
    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment&, const webrtc::SdpVideoFormat&) override {
        return nullptr;
    }
};

class CustomVideoDecoderFactory : public webrtc::VideoDecoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return {
            webrtc::SdpVideoFormat("VP8"),
            webrtc::SdpVideoFormat("H264")
        };
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
    signaling_thread_ = webrtc::Thread::Create();

    if (!network_thread_->Start() || !signaling_thread_->Start()) {
        std::cerr << "WebRTCManager: Failed to start WebRTC helper threads" << std::endl;
        webrtc::CleanupSSL();
        return false;
    }

    std::cout << "WebRTCManager: Creating PeerConnectionFactory..." << std::endl;
    
    auto audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
    auto audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
    auto video_encoder_factory = std::make_unique<CustomVideoEncoderFactory>();
    auto video_decoder_factory = std::make_unique<CustomVideoDecoderFactory>();

    factory_ = webrtc::CreatePeerConnectionFactory(
        network_thread_.get(),
        signaling_thread_.get(),
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
    std::cout << "WebRTCManager: Initialized successfully with Audio/Video pipelines!" << std::endl;
    return true;
}

void WebRTCManager::Deinitialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return;
    }

    std::cout << "WebRTCManager: Deinitializing factory and threads..." << std::endl;

    factory_ = nullptr;

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
    
    struct TaskParams {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        asio::any_io_executor executor;
        std::function<void(const std::string& sdp, const std::string& error)> callback;
    };
    auto* p = new TaskParams{pc, executor, callback};
    signaling_thread_->PostTask([p]() {
        auto observer = CreateSdpObserverProxy::Create(p->executor, p->callback);
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        p->pc->CreateOffer(observer.get(), options);
        delete p;
    });
}

void WebRTCManager::CreateAnswer(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
    asio::any_io_executor executor,
    std::function<void(const std::string& sdp, const std::string& error)> callback) {
    
    struct TaskParams {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        asio::any_io_executor executor;
        std::function<void(const std::string& sdp, const std::string& error)> callback;
    };
    auto* p = new TaskParams{pc, executor, callback};
    signaling_thread_->PostTask([p]() {
        auto observer = CreateSdpObserverProxy::Create(p->executor, p->callback);
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        p->pc->CreateAnswer(observer.get(), options);
        delete p;
    });
}

void WebRTCManager::SetRemoteDescription(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
    const std::string& type,
    const std::string& sdp,
    asio::any_io_executor executor,
    std::function<void(const std::string& error)> callback) {
    
    struct TaskParams {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        std::string type;
        std::string sdp;
        asio::any_io_executor executor;
        std::function<void(const std::string& error)> callback;
    };
    auto* p = new TaskParams{pc, type, sdp, executor, callback};
    signaling_thread_->PostTask([p]() {
        webrtc::SdpParseError err;
        webrtc::SdpType sdp_type = (p->type == "answer") ? webrtc::SdpType::kAnswer : webrtc::SdpType::kOffer;
        std::unique_ptr<webrtc::SessionDescriptionInterface> session_desc =
            webrtc::CreateSessionDescription(sdp_type, p->sdp, &err);
        
        if (!session_desc) {
            std::string err_msg = err.description;
            auto cb = p->callback;
            auto ex = p->executor;
            delete p;
            asio::post(ex, [cb, err_msg]() {
                cb(err_msg);
            });
            return;
        }

        auto observer = SetSdpObserverProxy::Create(p->executor, p->callback);
        p->pc->SetRemoteDescription(observer.get(), session_desc.release());
        delete p;
    });
}

void WebRTCManager::SetLocalDescription(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
    const std::string& type,
    const std::string& sdp,
    asio::any_io_executor executor,
    std::function<void(const std::string& error)> callback) {
    
    struct TaskParams {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        std::string type;
        std::string sdp;
        asio::any_io_executor executor;
        std::function<void(const std::string& error)> callback;
    };
    auto* p = new TaskParams{pc, type, sdp, executor, callback};
    signaling_thread_->PostTask([p]() {
        webrtc::SdpParseError err;
        webrtc::SdpType sdp_type = (p->type == "answer") ? webrtc::SdpType::kAnswer : webrtc::SdpType::kOffer;
        std::unique_ptr<webrtc::SessionDescriptionInterface> session_desc =
            webrtc::CreateSessionDescription(sdp_type, p->sdp, &err);
        
        if (!session_desc) {
            std::string err_msg = err.description;
            auto cb = p->callback;
            auto ex = p->executor;
            delete p;
            asio::post(ex, [cb, err_msg]() {
                cb(err_msg);
            });
            return;
        }

        auto observer = SetSdpObserverProxy::Create(p->executor, p->callback);
        p->pc->SetLocalDescription(observer.get(), session_desc.release());
        delete p;
    });
}

} // namespace livekit
