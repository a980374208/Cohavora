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


#include "api/environment/environment_factory.h"
#include "api/audio/audio_device.h"
#include "modules/video_coding/codecs/vp8/include/vp8.h"

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
            webrtc::SdpVideoFormat("VP8")
        };
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(
        const webrtc::Environment& env,
        const webrtc::SdpVideoFormat& format) override {
        return webrtc::CreateVp8Encoder(env);
    }
};

class CustomVideoDecoderFactory : public webrtc::VideoDecoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return {
            webrtc::SdpVideoFormat("VP8")
        };
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(
        const webrtc::Environment& env,
        const webrtc::SdpVideoFormat& format) override {
        return webrtc::CreateVp8Decoder(env);
    }
};

class DummyAudioDeviceModule : public webrtc::AudioDeviceModule {
public:
    DummyAudioDeviceModule() : playing_(false), recording_(false), audio_transport_(nullptr) {}

    ~DummyAudioDeviceModule() override {
        StopPlayout();
        StopRecording();
    }

    int32_t ActiveAudioLayer(AudioLayer* audioLayer) const override { *audioLayer = kDummyAudio; return 0; }
    int32_t RegisterAudioCallback(webrtc::AudioTransport* audioCallback) override {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_transport_ = audioCallback;
        return 0;
    }
    int32_t Init() override { return 0; }
    int32_t Terminate() override { 
        StopPlayout();
        return 0; 
    }
    bool Initialized() const override { return true; }
    int16_t PlayoutDevices() override { return 1; }
    int16_t RecordingDevices() override { return 0; }
    int32_t PlayoutDeviceName(uint16_t, char[webrtc::kAdmMaxDeviceNameSize], char[webrtc::kAdmMaxGuidSize]) override { return 0; }
    int32_t RecordingDeviceName(uint16_t, char[webrtc::kAdmMaxDeviceNameSize], char[webrtc::kAdmMaxGuidSize]) override { return 0; }
    int32_t SetPlayoutDevice(uint16_t) override { return 0; }
    int32_t SetPlayoutDevice(WindowsDeviceType) override { return 0; }
    int32_t SetRecordingDevice(uint16_t) override { return 0; }
    int32_t SetRecordingDevice(WindowsDeviceType) override { return 0; }
    int32_t PlayoutIsAvailable(bool* available) override { *available = true; return 0; }
    int32_t InitPlayout() override { return 0; }
    bool PlayoutIsInitialized() const override { return true; }
    int32_t RecordingIsAvailable(bool* available) override { *available = false; return 0; }
    int32_t InitRecording() override { return 0; }
    bool RecordingIsInitialized() const override { return true; }

    int32_t StartPlayout() override {
        if (playing_.exchange(true)) return 0;
        playout_thread_ = std::thread([this]() {
            int64_t elapsed_time_ms = 0;
            int64_t ntp_time_ms = 0;
            int16_t audio_buffer[480 * 2]; // 10ms at 48kHz stereo
            while (playing_.load()) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (audio_transport_) {
                        size_t samples_out = 0;
                        audio_transport_->NeedMorePlayData(480, sizeof(int16_t), 2, 48000, audio_buffer, samples_out, &elapsed_time_ms, &ntp_time_ms);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        return 0;
    }

    int32_t StopPlayout() override {
        if (!playing_.exchange(false)) return 0;
        if (playout_thread_.joinable()) {
            playout_thread_.join();
        }
        return 0;
    }

    bool Playing() const override { return playing_.load(); }
    int32_t StartRecording() override { return 0; }
    int32_t StopRecording() override { return 0; }
    bool Recording() const override { return false; }
    int32_t InitSpeaker() override { return 0; }
    bool SpeakerIsInitialized() const override { return true; }
    int32_t InitMicrophone() override { return 0; }
    bool MicrophoneIsInitialized() const override { return true; }
    int32_t SpeakerVolumeIsAvailable(bool* available) override { *available = false; return 0; }
    int32_t SetSpeakerVolume(uint32_t) override { return 0; }
    int32_t SpeakerVolume(uint32_t*) const override { return 0; }
    int32_t MaxSpeakerVolume(uint32_t*) const override { return 0; }
    int32_t MinSpeakerVolume(uint32_t*) const override { return 0; }
    int32_t MicrophoneVolumeIsAvailable(bool* available) override { *available = false; return 0; }
    int32_t SetMicrophoneVolume(uint32_t) override { return 0; }
    int32_t MicrophoneVolume(uint32_t*) const override { return 0; }
    int32_t MaxMicrophoneVolume(uint32_t*) const override { return 0; }
    int32_t MinMicrophoneVolume(uint32_t*) const override { return 0; }
    int32_t SpeakerMuteIsAvailable(bool* available) override { *available = false; return 0; }
    int32_t SetSpeakerMute(bool) override { return 0; }
    int32_t SpeakerMute(bool*) const override { return 0; }
    int32_t MicrophoneMuteIsAvailable(bool* available) override { *available = false; return 0; }
    int32_t SetMicrophoneMute(bool) override { return 0; }
    int32_t MicrophoneMute(bool*) const override { return 0; }
    int32_t StereoPlayoutIsAvailable(bool* available) const override { *available = true; return 0; }
    int32_t SetStereoPlayout(bool) override { return 0; }
    int32_t StereoPlayout(bool* enabled) const override { *enabled = true; return 0; }
    int32_t StereoRecordingIsAvailable(bool* available) const override { *available = false; return 0; }
    int32_t SetStereoRecording(bool) override { return 0; }
    int32_t StereoRecording(bool*) const override { return 0; }
    int32_t PlayoutDelay(uint16_t* delayMS) const override { *delayMS = 0; return 0; }
    bool BuiltInAECIsAvailable() const override { return false; }
    bool BuiltInAGCIsAvailable() const override { return false; }
    bool BuiltInNSIsAvailable() const override { return false; }
    int32_t EnableBuiltInAEC(bool) override { return 0; }
    int32_t EnableBuiltInAGC(bool) override { return 0; }
    int32_t EnableBuiltInNS(bool) override { return 0; }

private:
    std::atomic<bool> playing_;
    std::atomic<bool> recording_;
    mutable std::mutex mutex_;
    webrtc::AudioTransport* audio_transport_;
    std::thread playout_thread_;
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

    auto adm = webrtc::make_ref_counted<DummyAudioDeviceModule>();

    factory_ = webrtc::CreatePeerConnectionFactory(
        network_thread_.get(),
        signaling_thread_.get(),
        signaling_thread_.get(),
        adm,
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
