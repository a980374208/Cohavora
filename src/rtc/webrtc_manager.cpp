#include <winsock2.h>
#include <asio.hpp>
#include "webrtc_manager.h"
#include "rtc_base/ssl_adapter.h"
#include "api/create_peerconnection_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/sequence_checker.h"
#include <iostream>

#include "api/environment/environment_factory.h"
#include "api/audio/audio_device.h"
#include "api/audio/create_audio_device_module.h"
#include "modules/video_coding/codecs/vp8/include/vp8.h"
#include "modules/video_coding/codecs/vp9/include/vp9.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "media/engine/simulcast_encoder_adapter.h"
#include <objbase.h>

namespace webrtc {
namespace webrtc_checks_impl {
    void FatalLog(char const* file, int line) {}
}
}

namespace livekit {

namespace {

class SingleStreamVp8EncoderFactory : public webrtc::VideoEncoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return { webrtc::SdpVideoFormat("VP8") };
    }

    CodecSupport QueryCodecSupport(
        const webrtc::SdpVideoFormat& format,
        std::optional<std::string> scalability_mode) const override {
        CodecSupport support;
        support.is_supported = true;
        return support;
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(
        const webrtc::Environment& env,
        const webrtc::SdpVideoFormat& format) override {
        return webrtc::CreateVp8Encoder(env);
    }
};

class CustomVideoEncoderFactory : public webrtc::VideoEncoderFactory {
public:
    CustomVideoEncoderFactory()
        : internal_factory_(std::make_unique<SingleStreamVp8EncoderFactory>()) {}

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return internal_factory_->GetSupportedFormats();
    }

    CodecSupport QueryCodecSupport(
        const webrtc::SdpVideoFormat& format,
        std::optional<std::string> scalability_mode) const override {
        return internal_factory_->QueryCodecSupport(format, scalability_mode);
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(
        const webrtc::Environment& env,
        const webrtc::SdpVideoFormat& format) override {
        return std::make_unique<webrtc::SimulcastEncoderAdapter>(env, internal_factory_.get(), nullptr, format);
    }

private:
    std::unique_ptr<SingleStreamVp8EncoderFactory> internal_factory_;
};

class CustomVideoDecoderFactory : public webrtc::VideoDecoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        std::vector<webrtc::SdpVideoFormat> formats;
        formats.push_back(webrtc::SdpVideoFormat("VP8"));
        formats.push_back(webrtc::SdpVideoFormat("VP9"));
        if (webrtc::H264Decoder::IsSupported()) {
            for (const auto& f : webrtc::SupportedH264DecoderCodecs()) {
                formats.push_back(f);
            }
        }
        return formats;
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(
        const webrtc::Environment& env,
        const webrtc::SdpVideoFormat& format) override {
        if (_stricmp(format.name.c_str(), "VP8") == 0) {
            return webrtc::CreateVp8Decoder(env);
        }
        if (_stricmp(format.name.c_str(), "VP9") == 0) {
            return webrtc::VP9Decoder::Create();
        }
        if (_stricmp(format.name.c_str(), "H264") == 0) {
            return webrtc::H264Decoder::Create();
        }
        return nullptr;
    }
};

class PlayoutOnlyAudioDeviceModule : public webrtc::AudioDeviceModule {
public:
    explicit PlayoutOnlyAudioDeviceModule(webrtc::scoped_refptr<webrtc::AudioDeviceModule> inner)
        : inner_(inner) {}

    ~PlayoutOnlyAudioDeviceModule() override = default;

    int32_t ActiveAudioLayer(AudioLayer* audioLayer) const override {
        return inner_ ? inner_->ActiveAudioLayer(audioLayer) : -1;
    }

    int32_t RegisterAudioCallback(webrtc::AudioTransport* audioCallback) override {
        return inner_ ? inner_->RegisterAudioCallback(audioCallback) : -1;
    }

    int32_t Init() override {
        return inner_ ? inner_->Init() : 0;
    }

    int32_t Terminate() override {
        return inner_ ? inner_->Terminate() : 0;
    }

    bool Initialized() const override {
        return inner_ ? inner_->Initialized() : true;
    }

    // --- Playout 相关：100% 由原生 Core Audio 处理 ---
    int16_t PlayoutDevices() override {
        return inner_ ? inner_->PlayoutDevices() : 0;
    }

    int32_t PlayoutDeviceName(uint16_t index, char name[webrtc::kAdmMaxDeviceNameSize], char guid[webrtc::kAdmMaxGuidSize]) override {
        return inner_ ? inner_->PlayoutDeviceName(index, name, guid) : -1;
    }

    int32_t SetPlayoutDevice(uint16_t index) override {
        return inner_ ? inner_->SetPlayoutDevice(index) : 0;
    }

    int32_t SetPlayoutDevice(WindowsDeviceType device) override {
        return inner_ ? inner_->SetPlayoutDevice(device) : 0;
    }

    int32_t PlayoutIsAvailable(bool* available) override {
        return inner_ ? inner_->PlayoutIsAvailable(available) : 0;
    }

    int32_t InitPlayout() override {
        return inner_ ? inner_->InitPlayout() : 0;
    }

    bool PlayoutIsInitialized() const override {
        return inner_ ? inner_->PlayoutIsInitialized() : false;
    }

    int32_t StartPlayout() override {
        return inner_ ? inner_->StartPlayout() : 0;
    }

    int32_t StopPlayout() override {
        return inner_ ? inner_->StopPlayout() : 0;
    }

    bool Playing() const override {
        return inner_ ? inner_->Playing() : false;
    }

    int32_t InitSpeaker() override {
        return inner_ ? inner_->InitSpeaker() : 0;
    }

    bool SpeakerIsInitialized() const override {
        return inner_ ? inner_->SpeakerIsInitialized() : false;
    }

    int32_t SpeakerVolumeIsAvailable(bool* available) override {
        return inner_ ? inner_->SpeakerVolumeIsAvailable(available) : 0;
    }

    int32_t SetSpeakerVolume(uint32_t volume) override {
        return inner_ ? inner_->SetSpeakerVolume(volume) : 0;
    }

    int32_t SpeakerVolume(uint32_t* volume) const override {
        return inner_ ? inner_->SpeakerVolume(volume) : 0;
    }

    int32_t MaxSpeakerVolume(uint32_t* maxVolume) const override {
        return inner_ ? inner_->MaxSpeakerVolume(maxVolume) : 0;
    }

    int32_t MinSpeakerVolume(uint32_t* minVolume) const override {
        return inner_ ? inner_->MinSpeakerVolume(minVolume) : 0;
    }

    int32_t SpeakerMuteIsAvailable(bool* available) override {
        return inner_ ? inner_->SpeakerMuteIsAvailable(available) : 0;
    }

    int32_t SetSpeakerMute(bool enable) override {
        return inner_ ? inner_->SetSpeakerMute(enable) : 0;
    }

    int32_t SpeakerMute(bool* enabled) const override {
        return inner_ ? inner_->SpeakerMute(enabled) : 0;
    }

    int32_t StereoPlayoutIsAvailable(bool* available) const override {
        return inner_ ? inner_->StereoPlayoutIsAvailable(available) : 0;
    }

    int32_t SetStereoPlayout(bool enable) override {
        return inner_ ? inner_->SetStereoPlayout(enable) : 0;
    }

    int32_t StereoPlayout(bool* enabled) const override {
        return inner_ ? inner_->StereoPlayout(enabled) : 0;
    }

    int32_t PlayoutDelay(uint16_t* delayMS) const override {
        return inner_ ? inner_->PlayoutDelay(delayMS) : 0;
    }

    // --- Recording 相关：全部禁用，防止原生录音线程与自定义 WasapiAudioCapture / RtcAudioSource 冲突 ---
    int16_t RecordingDevices() override { return 0; }
    int32_t RecordingDeviceName(uint16_t, char[webrtc::kAdmMaxDeviceNameSize], char[webrtc::kAdmMaxGuidSize]) override { return -1; }
    int32_t SetRecordingDevice(uint16_t) override { return 0; }
    int32_t SetRecordingDevice(WindowsDeviceType) override { return 0; }
    int32_t RecordingIsAvailable(bool* available) override { if (available) *available = false; return 0; }
    int32_t InitRecording() override { return 0; }
    bool RecordingIsInitialized() const override { return false; }
    int32_t StartRecording() override { return 0; }
    int32_t StopRecording() override { return 0; }
    bool Recording() const override { return false; }
    int32_t InitMicrophone() override { return 0; }
    bool MicrophoneIsInitialized() const override { return false; }
    int32_t MicrophoneVolumeIsAvailable(bool* available) override { if (available) *available = false; return 0; }
    int32_t SetMicrophoneVolume(uint32_t) override { return 0; }
    int32_t MicrophoneVolume(uint32_t*) const override { return 0; }
    int32_t MaxMicrophoneVolume(uint32_t*) const override { return 0; }
    int32_t MinMicrophoneVolume(uint32_t*) const override { return 0; }
    int32_t MicrophoneMuteIsAvailable(bool* available) override { if (available) *available = false; return 0; }
    int32_t SetMicrophoneMute(bool) override { return 0; }
    int32_t MicrophoneMute(bool*) const override { return 0; }
    int32_t StereoRecordingIsAvailable(bool* available) const override { if (available) *available = false; return 0; }
    int32_t SetStereoRecording(bool) override { return 0; }
    int32_t StereoRecording(bool* enabled) const override { if (enabled) *enabled = false; return 0; }

    bool BuiltInAECIsAvailable() const override { return false; }
    bool BuiltInAGCIsAvailable() const override { return false; }
    bool BuiltInNSIsAvailable() const override { return false; }
    int32_t EnableBuiltInAEC(bool) override { return 0; }
    int32_t EnableBuiltInAGC(bool) override { return 0; }
    int32_t EnableBuiltInNS(bool) override { return 0; }

private:
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> inner_;
};

} // namespace

WebRTCManager& WebRTCManager::Instance() {
    static WebRTCManager instance;
    return instance;
}

WebRTCManager::~WebRTCManager() {
    Deinitialize();
}

#include "rtc_base/logging.h"
bool WebRTCManager::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return true;
    }

    // 确保当前线程启用 COM 多线程环境
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // 屏蔽 WebRTC 原生日志输出
    webrtc::LogMessage::LogToDebug(webrtc::LS_NONE);
    webrtc::LogMessage::SetLogToStderr(false);

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

    auto audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
    auto audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
    auto video_encoder_factory = std::make_unique<CustomVideoEncoderFactory>();
    auto video_decoder_factory = std::make_unique<CustomVideoDecoderFactory>();

    auto env = webrtc::CreateEnvironment();
    auto raw_adm = webrtc::CreateAudioDeviceModule(env, webrtc::AudioDeviceModule::kPlatformDefaultAudio);
    if (raw_adm) {
        adm_ = webrtc::make_ref_counted<PlayoutOnlyAudioDeviceModule>(raw_adm);
        worker_thread_->BlockingCall([this]() {
            if (adm_) {
                adm_->Init();
                // 自动绑定系统默认通信播放端点 (Default Communication Device / Default Console Device)
                int32_t ret = adm_->SetPlayoutDevice(webrtc::AudioDeviceModule::kDefaultCommunicationDevice);
                if (ret != 0) {
                    adm_->SetPlayoutDevice(webrtc::AudioDeviceModule::kDefaultDevice);
                }
                adm_->InitSpeaker();
                adm_->SetSpeakerVolume(255);
                adm_->SetSpeakerMute(false);
                adm_->InitPlayout();
            }
        });
        std::cout << "WebRTCManager: Native Platform Audio Device Module (Playout-Only ADM) initialized with Default Endpoint." << std::endl;
    }

    factory_ = webrtc::CreatePeerConnectionFactory(
        network_thread_.get(),
        worker_thread_.get(),
        signaling_thread_.get(),
        adm_,
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

    // 关键时序优化：在 PeerConnectionFactory 创建完成 (AudioTransport 已注册) 后，再启动播放渲染驱动！
    if (adm_) {
        worker_thread_->BlockingCall([this]() {
            if (adm_) {
                adm_->StartPlayout();
                std::cout << "WebRTCManager: ADM StartPlayout() activated AFTER PeerConnectionFactory & AudioTransport binding." << std::endl;
            }
        });
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

    // 1. 显式释放 PeerConnectionFactory
    factory_ = nullptr;

    // 2. 确保在 worker / signaling 线程执行完毕所有内部清理任务
    if (worker_thread_) {
        worker_thread_->BlockingCall([]() {});
    }
    if (signaling_thread_) {
        signaling_thread_->BlockingCall([]() {});
    }

    // 3. 显式终止并释放 ADM
    if (adm_) {
        if (worker_thread_) {
            worker_thread_->BlockingCall([this]() {
                adm_->Terminate();
                adm_ = nullptr;
            });
        } else {
            adm_->Terminate();
            adm_ = nullptr;
        }
    }

    // 4. 等待各线程所有剩余任务排空
    if (worker_thread_) {
        worker_thread_->BlockingCall([]() {});
    }
    if (signaling_thread_) {
        signaling_thread_->BlockingCall([]() {});
    }
    if (network_thread_) {
        network_thread_->BlockingCall([]() {});
    }

    // 5. 停止并释放辅助线程
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
