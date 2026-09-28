#include <winsock2.h>
#include <asio.hpp>
#include "webrtc_manager.h"
#include "telemetry/diagnostic_pipeline.h"
#include "telemetry/sdp_negotiation_trace.h"
#include "core/executor_lifetime.h"
#include "audio_playout_device_selection.h"
#include "audio_playout_warmup.h"
#include "media/audio_apm.h"
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

#include "api/environment/environment_factory.h"
#include "api/audio/audio_device.h"
#include "api/audio/create_audio_device_module.h"
#include "modules/video_coding/codecs/vp8/include/vp8.h"
#include "modules/video_coding/codecs/vp9/include/vp9.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/codecs/av1/dav1d_decoder.h"
#include "modules/video_coding/codecs/av1/libaom_av1_encoder.h"
#include "modules/video_coding/svc/scalability_mode_util.h"
#include "media/engine/simulcast_encoder_adapter.h"
#include <objbase.h>

namespace webrtc {
namespace webrtc_checks_impl {
    void FatalLog(char const* file, int line) {}
}
}

namespace livekit {

namespace {

void EmitRtcLifecycle(diagnostic::RtcStatus status) noexcept {
    diagnostic::Event event;
    event.kind = diagnostic::EventKind::RtcLifecycle;
    event.thread_role = diagnostic::ThreadRole::Rtc;
    event.rtc_status = status;
    diagnostic::EmitBusinessEvent(event);
}

class SingleStreamVideoEncoderFactory : public webrtc::VideoEncoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        std::vector<webrtc::SdpVideoFormat> formats{
            webrtc::SdpVideoFormat("VP8")};
        if (webrtc::H264Encoder::IsSupported()) {
            auto h264 = webrtc::SupportedH264Codecs();
            formats.insert(formats.end(), h264.begin(), h264.end());
        }
        return formats;
    }

    CodecSupport QueryCodecSupport(
        const webrtc::SdpVideoFormat& format,
        std::optional<std::string> scalability_mode) const override {
        CodecSupport support;
        support.is_supported = _stricmp(format.name.c_str(), "VP8") == 0 ||
            (_stricmp(format.name.c_str(), "H264") == 0 &&
             webrtc::H264Encoder::IsSupported());
        return support;
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(
        const webrtc::Environment& env,
        const webrtc::SdpVideoFormat& format) override {
        if (_stricmp(format.name.c_str(), "VP8") == 0) {
            return webrtc::CreateVp8Encoder(env);
        }
        if (_stricmp(format.name.c_str(), "H264") == 0 &&
            webrtc::H264Encoder::IsSupported()) {
            return webrtc::CreateH264Encoder(
                env, webrtc::H264EncoderSettings::Parse(format));
        }
        return nullptr;
    }
};

class CustomVideoEncoderFactory : public webrtc::VideoEncoderFactory {
public:
    CustomVideoEncoderFactory()
        : simulcast_factory_(std::make_unique<SingleStreamVideoEncoderFactory>()) {}

    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        std::vector<webrtc::SdpVideoFormat> formats;

        // Preserve the established VP8/H264 preference order. VP9 and AV1 use
        // their native encoder SVC implementation instead of the simulcast adapter.
        const auto legacy_formats = simulcast_factory_->GetSupportedFormats();
        AppendFormatsNamed(formats, legacy_formats, "VP8");
        AppendFormatsNamed(formats, legacy_formats, "H264");
        const auto vp9_formats = webrtc::SupportedVP9Codecs(false);
        formats.insert(formats.end(), vp9_formats.begin(), vp9_formats.end());
        formats.push_back(webrtc::SdpVideoFormat::AV1Profile0());
        return formats;
    }

    CodecSupport QueryCodecSupport(
        const webrtc::SdpVideoFormat& format,
        std::optional<std::string> scalability_mode) const override {
        if (CodecNameEquals(format, "VP8") || CodecNameEquals(format, "H264")) {
            return simulcast_factory_->QueryCodecSupport(format, scalability_mode);
        }
        if (CodecNameEquals(format, "VP9")) {
            const auto supported_formats = webrtc::SupportedVP9Codecs(false);
            if (!format.IsCodecInList(supported_formats)) return {};
            if (!scalability_mode.has_value()) return {.is_supported = true};
            const auto parsed_mode = webrtc::ScalabilityModeFromString(*scalability_mode);
            return {.is_supported = parsed_mode.has_value() &&
                webrtc::VP9Encoder::SupportsScalabilityMode(*parsed_mode)};
        }
        if (CodecNameEquals(format, "AV1")) {
            const std::vector<webrtc::SdpVideoFormat> supported{
                webrtc::SdpVideoFormat::AV1Profile0()};
            return {
                .is_supported = format.IsCodecInList(supported) &&
                    (!scalability_mode.has_value() || *scalability_mode == "L1T1")};
        }
        return {};
    }

    std::unique_ptr<webrtc::VideoEncoder> Create(
        const webrtc::Environment& env,
        const webrtc::SdpVideoFormat& format) override {
        if (CodecNameEquals(format, "VP8") || CodecNameEquals(format, "H264")) {
            return std::make_unique<webrtc::SimulcastEncoderAdapter>(
                env, simulcast_factory_.get(), nullptr, format);
        }
        if (CodecNameEquals(format, "AV1")) {
            const std::vector<webrtc::SdpVideoFormat> supported{
                webrtc::SdpVideoFormat::AV1Profile0()};
            if (!format.IsCodecInList(supported)) return nullptr;
            return webrtc::CreateLibaomAv1Encoder(env);
        }
        if (CodecNameEquals(format, "VP9")) {
            const auto supported_formats = webrtc::SupportedVP9Codecs(false);
            if (!format.IsCodecInList(supported_formats)) return nullptr;
            const auto profile = webrtc::ParseSdpForVP9Profile(format.parameters)
                .value_or(webrtc::VP9Profile::kProfile0);
            return webrtc::CreateVp9Encoder(env, {.profile = profile});
        }
        return nullptr;
    }

private:
    static bool CodecNameEquals(
        const webrtc::SdpVideoFormat& format,
        const char* expected) {
        return _stricmp(format.name.c_str(), expected) == 0;
    }

    static void AppendFormatsNamed(
        std::vector<webrtc::SdpVideoFormat>& destination,
        const std::vector<webrtc::SdpVideoFormat>& source,
        const char* codec_name) {
        for (const auto& format : source) {
            if (CodecNameEquals(format, codec_name)) {
                destination.push_back(format);
            }
        }
    }

    std::unique_ptr<webrtc::VideoEncoderFactory> simulcast_factory_;
};

class CustomVideoDecoderFactory : public webrtc::VideoDecoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        std::vector<webrtc::SdpVideoFormat> formats;
        formats.push_back(webrtc::SdpVideoFormat("VP8"));
        for (const auto& format : webrtc::SupportedVP9DecoderCodecs()) {
            formats.push_back(format);
        }
        if (webrtc::H264Decoder::IsSupported()) {
            for (const auto& f : webrtc::SupportedH264DecoderCodecs()) {
                formats.push_back(f);
            }
        }
        formats.push_back(webrtc::SdpVideoFormat::AV1Profile0());
        formats.push_back(webrtc::SdpVideoFormat::AV1Profile1());
        return formats;
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(
        const webrtc::Environment& env,
        const webrtc::SdpVideoFormat& format) override {
        if (_stricmp(format.name.c_str(), "VP8") == 0) {
            return webrtc::CreateVp8Decoder(env);
        }
        if (_stricmp(format.name.c_str(), "VP9") == 0) {
            const auto supported_formats = webrtc::SupportedVP9DecoderCodecs();
            if (!format.IsCodecInList(supported_formats)) return nullptr;
            return webrtc::VP9Decoder::Create();
        }
        if (_stricmp(format.name.c_str(), "H264") == 0) {
            return webrtc::H264Decoder::Create();
        }
        if (_stricmp(format.name.c_str(), "AV1") == 0) {
            const std::vector<webrtc::SdpVideoFormat> supported{
                webrtc::SdpVideoFormat::AV1Profile0(),
                webrtc::SdpVideoFormat::AV1Profile1()};
            if (!format.IsCodecInList(supported)) return nullptr;
            return webrtc::CreateDav1dDecoder(env);
        }
        return nullptr;
    }
};

class PlayoutAudioTransportWrapper : public webrtc::AudioTransport {
public:
    explicit PlayoutAudioTransportWrapper(webrtc::AudioTransport* inner)
        : inner_(inner) {}

    void ResetWarmup() {
        warmup_.Reset();
    }

    int32_t RecordedDataIsAvailable(const void* audioSamples,
                                    size_t nSamples,
                                    size_t nBytesPerSample,
                                    size_t nChannels,
                                    uint32_t samplesPerSec,
                                    uint32_t totalDelayMS,
                                    int32_t clockDrift,
                                    uint32_t currentMicLevel,
                                    bool keyPressed,
                                    uint32_t& newMicLevel) override {
        if (inner_) {
            return inner_->RecordedDataIsAvailable(audioSamples, nSamples, nBytesPerSample,
                                                   nChannels, samplesPerSec, totalDelayMS,
                                                   clockDrift, currentMicLevel, keyPressed, newMicLevel);
        }
        return 0;
    }

    int32_t NeedMorePlayData(size_t nSamples,
                             size_t nBytesPerSample,
                             size_t nChannels,
                             uint32_t samplesPerSec,
                             void* audioSamples,
                             size_t& nSamplesOut,
                             int64_t* elapsed_time_ms,
                             int64_t* ntp_time_ms) override {
        if (!inner_) {
            nSamplesOut = 0;
            return -1;
        }
        int32_t res = inner_->NeedMorePlayData(nSamples, nBytesPerSample, nChannels,
                                               samplesPerSec, audioSamples, nSamplesOut,
                                               elapsed_time_ms, ntp_time_ms);
        if (res == 0 && audioSamples && nSamplesOut > 0 &&
            nBytesPerSample == sizeof(int16_t)) {
            const size_t output_frames = std::min(nSamplesOut, nSamples);
            const size_t output_samples = output_frames * nChannels;
            warmup_.Process(static_cast<int16_t*>(audioSamples), output_samples,
                            nChannels, samplesPerSec);

            // 将下行扬声器渲染音频作为反向参考信号送入 APM (AEC 回声消除)
            auto apm = WebRTCManager::Instance().apm_processor();
            if (apm) {
                const int16_t* pcm = static_cast<const int16_t*>(audioSamples);
                std::vector<int16_t> render_pcm(pcm, pcm + output_samples);
                AudioFrame render_frame(std::move(render_pcm),
                                        static_cast<int>(samplesPerSec),
                                        static_cast<int>(nChannels),
                                        static_cast<int>(output_frames));
                apm->ProcessRenderFrame(render_frame);
            }
        }
        return res;
    }

    void PullRenderData(int bits_per_sample,
                        int sample_rate,
                        size_t number_of_channels,
                        size_t number_of_frames,
                        void* audio_data,
                        int64_t* elapsed_time_ms,
                        int64_t* ntp_time_ms) override {
        if (inner_) {
            inner_->PullRenderData(bits_per_sample, sample_rate, number_of_channels,
                                   number_of_frames, audio_data, elapsed_time_ms, ntp_time_ms);
            if (bits_per_sample == 16 && audio_data) {
                const size_t total_samples = number_of_frames * number_of_channels;
                warmup_.Process(static_cast<int16_t*>(audio_data),
                                total_samples,
                                number_of_channels,
                                static_cast<uint32_t>(sample_rate));

                auto apm = WebRTCManager::Instance().apm_processor();
                if (apm) {
                    const int16_t* pcm = static_cast<const int16_t*>(audio_data);
                    std::vector<int16_t> render_pcm(pcm, pcm + total_samples);
                    AudioFrame render_frame(std::move(render_pcm),
                                            sample_rate,
                                            static_cast<int>(number_of_channels),
                                            static_cast<int>(number_of_frames));
                    apm->ProcessRenderFrame(render_frame);
                }
            }
        }
    }

private:
    webrtc::AudioTransport* inner_;
    AudioPlayoutWarmup warmup_;
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
        if (!inner_) return -1;
        if (audioCallback) {
            auto wrapper = std::make_unique<PlayoutAudioTransportWrapper>(audioCallback);
            const int32_t result = inner_->RegisterAudioCallback(wrapper.get());
            if (result == 0) {
                transport_wrapper_ = std::move(wrapper);
            }
            return result;
        }

        // The native render thread retains this raw pointer. Unregister it
        // before releasing the wrapper, otherwise teardown can race a callback.
        const int32_t result = inner_->RegisterAudioCallback(nullptr);
        if (result == 0) {
            transport_wrapper_.reset();
        }
        return result;
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
        if (transport_wrapper_) transport_wrapper_->ResetWarmup();
        return inner_ ? inner_->SetPlayoutDevice(index) : 0;
    }

    int32_t SetPlayoutDevice(WindowsDeviceType device) override {
        if (transport_wrapper_) transport_wrapper_->ResetWarmup();
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
        if (transport_wrapper_) transport_wrapper_->ResetWarmup();
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
    std::unique_ptr<PlayoutAudioTransportWrapper> transport_wrapper_;
};

} // namespace

std::unique_ptr<webrtc::VideoEncoderFactory> CreateVideoEncoderFactory() {
    return std::make_unique<CustomVideoEncoderFactory>();
}

std::unique_ptr<webrtc::VideoDecoderFactory> CreateVideoDecoderFactory() {
    return std::make_unique<CustomVideoDecoderFactory>();
}

bool IsVideoEncoderFormatSupported(
    const std::string& codec_name,
    const std::string& scalability_mode) {
    auto factory = CreateVideoEncoderFactory();
    const auto formats = factory->GetSupportedFormats();
    for (const auto& format : formats) {
        if (_stricmp(format.name.c_str(), codec_name.c_str()) != 0) continue;
        const auto mode = scalability_mode.empty()
            ? std::optional<std::string>{}
            : std::optional<std::string>{scalability_mode};
        if (factory->QueryCodecSupport(format, mode).is_supported) return true;
    }
    return false;
}

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

    EmitRtcLifecycle(diagnostic::RtcStatus::Starting);
    if (!webrtc::InitializeSSL()) {
        EmitRtcLifecycle(diagnostic::RtcStatus::SslFailed);
        return false;
    }

    network_thread_ = webrtc::Thread::CreateWithSocketServer();
    worker_thread_ = webrtc::Thread::Create();
    signaling_thread_ = webrtc::Thread::Create();

    if (!network_thread_->Start() || !worker_thread_->Start() || !signaling_thread_->Start()) {
        EmitRtcLifecycle(diagnostic::RtcStatus::ThreadsFailed);
        webrtc::CleanupSSL();
        return false;
    }

    auto audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
    auto audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
    auto video_encoder_factory = CreateVideoEncoderFactory();
    auto video_decoder_factory = CreateVideoDecoderFactory();

    auto env = webrtc::CreateEnvironment();
    auto raw_adm = webrtc::CreateAudioDeviceModule(env, webrtc::AudioDeviceModule::kPlatformDefaultAudio);
    if (raw_adm) {
        adm_ = webrtc::make_ref_counted<PlayoutOnlyAudioDeviceModule>(raw_adm);
        worker_thread_->BlockingCall([this]() {
            if (adm_) {
                adm_->Init();
                // Use the same Windows default playback role as settings and
                // speaker testing, rather than the separate communications role.
                if (adm_->SetPlayoutDevice(webrtc::AudioDeviceModule::kDefaultDevice) != 0) {
                    EmitRtcLifecycle(diagnostic::RtcStatus::PlayoutDeviceFailed);
                }
            }
        });
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
        EmitRtcLifecycle(diagnostic::RtcStatus::FactoryFailed);
        Deinitialize();
        return false;
    }

    // The factory registers VoiceEngine's AudioTransport with the ADM. Start
    // rendering only after that callback is installed; the warmup wrapper
    // suppresses the first discontinuous buffers without muting real audio.
    if (adm_) {
        worker_thread_->BlockingCall([this]() {
            if (!adm_) return;
            const int32_t speaker_result = adm_->InitSpeaker();
            const int32_t playout_result = adm_->InitPlayout();
            const int32_t start_result = adm_->Playing() ? 0 : adm_->StartPlayout();
            if (speaker_result != 0 || playout_result != 0 || start_result != 0) {
                EmitRtcLifecycle(diagnostic::RtcStatus::PlayoutStartFailed);
            }
        });
    }

    initialized_ = true;
    EmitRtcLifecycle(diagnostic::RtcStatus::Ready);
    return true;
}

bool WebRTCManager::SetPlayoutDevice(uint16_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!adm_ || !worker_thread_) return false;
    return worker_thread_->BlockingCall([this, index]() {
        return detail::ApplyPlayoutDevice(*adm_, index, [this] { ResetApmProcessor(); });
    });
}

bool WebRTCManager::SetPlayoutDeviceById(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!adm_ || !worker_thread_) return false;
    // BlockingCall completes before device_id goes out of scope. Keep its
    // closure small and trivially copyable across the packaged WebRTC ABI.
    return worker_thread_->BlockingCall([this, &device_id]() {
        return detail::SelectPlayoutDeviceById(*adm_, device_id, [this] { ResetApmProcessor(); });
    });
}

bool WebRTCManager::EnsurePlayout() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!adm_ || !worker_thread_) return false;
    return worker_thread_->BlockingCall([this]() {
        if (adm_->Playing()) return true;
        if (!adm_->PlayoutIsInitialized()
                && (adm_->InitSpeaker() != 0 || adm_->InitPlayout() != 0)) return false;
        return adm_->StartPlayout() == 0 && adm_->Playing();
    });
}

void WebRTCManager::Deinitialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return;
    }

    // Stop native callbacks before destroying the VoiceEngine/AudioTransport.
    if (adm_) {
        if (worker_thread_) {
            worker_thread_->BlockingCall([this]() {
                if (adm_->Playing()) {
                    adm_->StopPlayout();
                }
                if (adm_->Recording()) {
                    adm_->StopRecording();
                }
            });
        } else {
            if (adm_->Playing()) {
                adm_->StopPlayout();
            }
            if (adm_->Recording()) {
                adm_->StopRecording();
            }
        }
    }

    factory_ = nullptr;

    if (adm_) {
        if (worker_thread_) {
            worker_thread_->BlockingCall([this]() {
                adm_->RegisterAudioCallback(nullptr);
                adm_->Terminate();
                adm_ = nullptr;
            });
        } else {
            adm_->RegisterAudioCallback(nullptr);
            adm_->Terminate();
            adm_ = nullptr;
        }
    }

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
    EmitRtcLifecycle(diagnostic::RtcStatus::Stopped);
}

using CreateSdpCompletion = CancellableExecutorCallback<std::string, std::string>;
using SetSdpCompletion = CancellableExecutorCallback<std::string>;

// All state reads are on the WebRTC signaling thread, at the actual call/callback.
diagnostic::SdpState SdpStateOf(webrtc::PeerConnectionInterface* pc) {
    using P = webrtc::PeerConnectionInterface;
    using S = diagnostic::SdpState;
    switch (pc->signaling_state()) {
    case P::kStable: return S::Stable;
    case P::kHaveLocalOffer: return S::HaveLocalOffer;
    case P::kHaveLocalPrAnswer: return S::HaveLocalPranswer;
    case P::kHaveRemoteOffer: return S::HaveRemoteOffer;
    case P::kHaveRemotePrAnswer: return S::HaveRemotePranswer;
    case P::kClosed: return S::Closed;
    }
    return S::Unknown;
}

class RtcSdpOperation final {
public:
    RtcSdpOperation(webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
                    std::shared_ptr<diagnostic::SdpNegotiationTrace> trace,
                    diagnostic::SdpAction action,
                    diagnostic::SdpDescription description = diagnostic::SdpDescription::Unknown)
        : pc_(std::move(pc)), trace_(std::move(trace)), action_(action), description_(description), before_(SdpStateOf(pc_.get())) {
        if (trace_) trace_->Record(action_, diagnostic::SdpPhase::Started,
            before_, before_, diagnostic::SdpReason::None, 0, diagnostic::ThreadRole::Rtc, description_);
    }
    void Complete(diagnostic::SdpPhase phase, diagnostic::SdpReason reason = diagnostic::SdpReason::None,
                  int error = 0) {
        if (!trace_) return;
        trace_->Record(action_, phase, before_, SdpStateOf(pc_.get()), reason, error,
            diagnostic::ThreadRole::Rtc, description_);
        if (phase == diagnostic::SdpPhase::Failed)
            trace_->Finish(diagnostic::Outcome::Failure, reason);
        if (phase == diagnostic::SdpPhase::Rejected)
            trace_->Finish(diagnostic::Outcome::Cancelled, reason);
    }
    bool traced() const { return bool(trace_); }
private:
    const webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;
    const std::shared_ptr<diagnostic::SdpNegotiationTrace> trace_;
    const diagnostic::SdpAction action_;
    const diagnostic::SdpDescription description_;
    const diagnostic::SdpState before_;
};

class CreateSdpObserverProxy : public webrtc::CreateSessionDescriptionObserver {
public:
    static webrtc::scoped_refptr<CreateSdpObserverProxy> Create(
        std::shared_ptr<CreateSdpCompletion> completion,
        diagnostic::Stage stage, std::shared_ptr<RtcSdpOperation> operation) {
        return webrtc::make_ref_counted<CreateSdpObserverProxy>(
            std::move(completion), stage, std::move(operation));
    }

    CreateSdpObserverProxy(
        std::shared_ptr<CreateSdpCompletion> completion,
        diagnostic::Stage stage, std::shared_ptr<RtcSdpOperation> operation)
        : completion_(std::move(completion)), stage_(stage), operation_(std::move(operation)) {}

    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
        const bool pending = completion_->pending();
        operation_->Complete(pending ? diagnostic::SdpPhase::Completed : diagnostic::SdpPhase::Rejected,
            pending ? diagnostic::SdpReason::None : diagnostic::SdpReason::GenerationExpired);
        std::string sdp;
        desc->ToString(&sdp);
        delete desc;
        completion_->Complete(std::move(sdp), "");
    }

    void OnFailure(webrtc::RTCError error) override {
        const bool pending = completion_->pending();
        operation_->Complete(pending ? diagnostic::SdpPhase::Failed : diagnostic::SdpPhase::Rejected,
            pending ? diagnostic::SdpReason::RtcError : diagnostic::SdpReason::GenerationExpired,
            static_cast<int>(error.type()));
        diagnostic::Event event;
        event.kind = diagnostic::EventKind::RtcSdpFailed;
        event.thread_role = diagnostic::ThreadRole::Rtc;
        event.stage = stage_;
        event.error_layer = diagnostic::ErrorLayer::Rtc;
        event.rtc_error_type = static_cast<int>(error.type());
        if (!operation_->traced()) diagnostic::EmitBusinessEvent(event);
        std::string err_msg = error.message();
        completion_->Complete("", std::move(err_msg));
    }

private:
    std::shared_ptr<CreateSdpCompletion> completion_;
    diagnostic::Stage stage_;
    std::shared_ptr<RtcSdpOperation> operation_;
};

class SetSdpObserverProxy : public webrtc::SetSessionDescriptionObserver {
public:
    static webrtc::scoped_refptr<SetSdpObserverProxy> Create(
        std::shared_ptr<SetSdpCompletion> completion,
        diagnostic::Stage stage, std::shared_ptr<RtcSdpOperation> operation) {
        return webrtc::make_ref_counted<SetSdpObserverProxy>(
            std::move(completion), stage, std::move(operation));
    }

    SetSdpObserverProxy(
        std::shared_ptr<SetSdpCompletion> completion,
        diagnostic::Stage stage, std::shared_ptr<RtcSdpOperation> operation)
        : completion_(std::move(completion)), stage_(stage), operation_(std::move(operation)) {}

    void OnSuccess() override {
        const bool pending = completion_->pending();
        operation_->Complete(pending ? diagnostic::SdpPhase::Completed : diagnostic::SdpPhase::Rejected,
            pending ? diagnostic::SdpReason::None : diagnostic::SdpReason::GenerationExpired);
        completion_->Complete("");
    }

    void OnFailure(webrtc::RTCError error) override {
        const bool pending = completion_->pending();
        operation_->Complete(pending ? diagnostic::SdpPhase::Failed : diagnostic::SdpPhase::Rejected,
            pending ? diagnostic::SdpReason::RtcError : diagnostic::SdpReason::GenerationExpired,
            static_cast<int>(error.type()));
        diagnostic::Event event;
        event.kind = diagnostic::EventKind::RtcSdpFailed;
        event.thread_role = diagnostic::ThreadRole::Rtc;
        event.stage = stage_;
        event.error_layer = diagnostic::ErrorLayer::Rtc;
        event.rtc_error_type = static_cast<int>(error.type());
        if (!operation_->traced()) diagnostic::EmitBusinessEvent(event);
        std::string err_msg = error.message();
        completion_->Complete(std::move(err_msg));
    }

private:
    std::shared_ptr<SetSdpCompletion> completion_;
    diagnostic::Stage stage_;
    std::shared_ptr<RtcSdpOperation> operation_;
};

void WebRTCManager::CreateOffer(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
    asio::any_io_executor executor,
    std::function<void(const std::string& sdp, const std::string& error)> callback,
    bool ice_restart,
    std::shared_ptr<ExecutorCallbackGate> callback_gate,
    std::shared_ptr<diagnostic::SdpNegotiationTrace> trace) {
    if (!callback_gate) callback_gate = std::make_shared<ExecutorCallbackGate>(executor);
    auto completion = CreateSdpCompletion::Create(
        std::move(callback_gate), std::move(callback), "", "operation cancelled");
    
    struct TaskParams {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        std::shared_ptr<CreateSdpCompletion> completion;
        std::shared_ptr<diagnostic::SdpNegotiationTrace> trace;
        bool ice_restart;
    };
    auto* p = new TaskParams{pc, std::move(completion), std::move(trace), ice_restart};
    // Keep the cross-library task capture pointer-only, as in the existing ABI boundary.
    signaling_thread_->PostTask([p]() {
        std::unique_ptr<TaskParams> owned(p);
        const auto& trace = p->trace;
        if (!p->completion->pending()) {
            if (trace) {
                trace->Record(diagnostic::SdpAction::CreateOffer, diagnostic::SdpPhase::Rejected,
                    diagnostic::SdpState::Unknown, diagnostic::SdpState::Unknown,
                    diagnostic::SdpReason::GenerationExpired, 0, diagnostic::ThreadRole::Rtc);
                trace->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::GenerationExpired);
            }
            return;
        }
        auto operation = std::make_shared<RtcSdpOperation>(p->pc, trace, diagnostic::SdpAction::CreateOffer);
        auto observer = CreateSdpObserverProxy::Create(
            p->completion, diagnostic::Stage::CreateOffer, operation);
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        options.ice_restart = p->ice_restart;
        p->pc->CreateOffer(observer.get(), options);
    });
}

void WebRTCManager::CreateAnswer(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
    asio::any_io_executor executor,
    std::function<void(const std::string& sdp, const std::string& error)> callback,
    std::shared_ptr<ExecutorCallbackGate> callback_gate,
    std::shared_ptr<diagnostic::SdpNegotiationTrace> trace) {
    if (!callback_gate) callback_gate = std::make_shared<ExecutorCallbackGate>(executor);
    auto completion = CreateSdpCompletion::Create(
        std::move(callback_gate), std::move(callback), "", "operation cancelled");
    
    struct TaskParams {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        std::shared_ptr<CreateSdpCompletion> completion;
        std::shared_ptr<diagnostic::SdpNegotiationTrace> trace;
    };
    auto* p = new TaskParams{pc, std::move(completion), std::move(trace)};
    // Keep the cross-library task capture pointer-only, as in the existing ABI boundary.
    signaling_thread_->PostTask([p]() {
        std::unique_ptr<TaskParams> owned(p);
        const auto& trace = p->trace;
        if (!p->completion->pending()) {
            if (trace) {
                trace->Record(diagnostic::SdpAction::CreateAnswer, diagnostic::SdpPhase::Rejected,
                    diagnostic::SdpState::Unknown, diagnostic::SdpState::Unknown,
                    diagnostic::SdpReason::GenerationExpired, 0, diagnostic::ThreadRole::Rtc);
                trace->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::GenerationExpired);
            }
            return;
        }
        auto operation = std::make_shared<RtcSdpOperation>(p->pc, trace, diagnostic::SdpAction::CreateAnswer);
        auto observer = CreateSdpObserverProxy::Create(
            p->completion, diagnostic::Stage::CreateAnswer, operation);
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        p->pc->CreateAnswer(observer.get(), options);
    });
}

void WebRTCManager::SetRemoteDescription(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
    const std::string& type,
    const std::string& sdp,
    asio::any_io_executor executor,
    std::function<void(const std::string& error)> callback,
    std::shared_ptr<ExecutorCallbackGate> callback_gate,
    std::shared_ptr<diagnostic::SdpNegotiationTrace> trace) {
    if (!callback_gate) callback_gate = std::make_shared<ExecutorCallbackGate>(executor);
    auto completion = SetSdpCompletion::Create(
        std::move(callback_gate), std::move(callback), "operation cancelled");
    
    struct TaskParams {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        std::string type;
        std::string sdp;
        std::shared_ptr<SetSdpCompletion> completion;
        std::shared_ptr<diagnostic::SdpNegotiationTrace> trace;
    };
    auto* p = new TaskParams{pc, type, sdp, std::move(completion), std::move(trace)};
    // Keep the cross-library task capture pointer-only, as in the existing ABI boundary.
    signaling_thread_->PostTask([p]() {
        std::unique_ptr<TaskParams> owned(p);
        const auto& trace = p->trace;
        if (!p->completion->pending()) {
            if (trace) {
                trace->Record(diagnostic::SdpAction::SetRemote, diagnostic::SdpPhase::Rejected,
                    diagnostic::SdpState::Unknown, diagnostic::SdpState::Unknown,
                    diagnostic::SdpReason::GenerationExpired, 0, diagnostic::ThreadRole::Rtc);
                trace->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::GenerationExpired);
            }
            return;
        }
        auto operation = std::make_shared<RtcSdpOperation>(p->pc, trace, diagnostic::SdpAction::SetRemote,
            p->type == "answer" ? diagnostic::SdpDescription::Answer : diagnostic::SdpDescription::Offer);
        webrtc::SdpParseError err;
        webrtc::SdpType sdp_type = (p->type == "answer") ? webrtc::SdpType::kAnswer : webrtc::SdpType::kOffer;
        std::unique_ptr<webrtc::SessionDescriptionInterface> session_desc =
            webrtc::CreateSessionDescription(sdp_type, p->sdp, &err);
        
        if (!session_desc) {
            operation->Complete(diagnostic::SdpPhase::Failed, diagnostic::SdpReason::ParseError);
            std::string err_msg = err.description;
            p->completion->Complete(std::move(err_msg));
            return;
        }

        auto observer = SetSdpObserverProxy::Create(
            p->completion, diagnostic::Stage::SetRemoteDescription, operation);
        p->pc->SetRemoteDescription(observer.get(), session_desc.release());
    });
}

void WebRTCManager::SetLocalDescription(
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
    const std::string& type,
    const std::string& sdp,
    asio::any_io_executor executor,
    std::function<void(const std::string& error)> callback,
    std::shared_ptr<ExecutorCallbackGate> callback_gate,
    std::shared_ptr<diagnostic::SdpNegotiationTrace> trace) {
    if (!callback_gate) callback_gate = std::make_shared<ExecutorCallbackGate>(executor);
    auto completion = SetSdpCompletion::Create(
        std::move(callback_gate), std::move(callback), "operation cancelled");
    
    struct TaskParams {
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
        std::string type;
        std::string sdp;
        std::shared_ptr<SetSdpCompletion> completion;
        std::shared_ptr<diagnostic::SdpNegotiationTrace> trace;
    };
    auto* p = new TaskParams{pc, type, sdp, std::move(completion), std::move(trace)};
    // Keep the cross-library task capture pointer-only, as in the existing ABI boundary.
    signaling_thread_->PostTask([p]() {
        std::unique_ptr<TaskParams> owned(p);
        const auto& trace = p->trace;
        if (!p->completion->pending()) {
            if (trace) {
                trace->Record(diagnostic::SdpAction::SetLocal, diagnostic::SdpPhase::Rejected,
                    diagnostic::SdpState::Unknown, diagnostic::SdpState::Unknown,
                    diagnostic::SdpReason::GenerationExpired, 0, diagnostic::ThreadRole::Rtc);
                trace->Finish(diagnostic::Outcome::Cancelled, diagnostic::SdpReason::GenerationExpired);
            }
            return;
        }
        auto operation = std::make_shared<RtcSdpOperation>(p->pc, trace, diagnostic::SdpAction::SetLocal,
            p->type == "answer" ? diagnostic::SdpDescription::Answer : diagnostic::SdpDescription::Offer);
        webrtc::SdpParseError err;
        webrtc::SdpType sdp_type = (p->type == "answer") ? webrtc::SdpType::kAnswer : webrtc::SdpType::kOffer;
        std::unique_ptr<webrtc::SessionDescriptionInterface> session_desc =
            webrtc::CreateSessionDescription(sdp_type, p->sdp, &err);
        
        if (!session_desc) {
            operation->Complete(diagnostic::SdpPhase::Failed, diagnostic::SdpReason::ParseError);
            std::string err_msg = err.description;
            p->completion->Complete(std::move(err_msg));
            return;
        }

        auto observer = SetSdpObserverProxy::Create(
            p->completion, diagnostic::Stage::SetLocalDescription, operation);
        p->pc->SetLocalDescription(observer.get(), session_desc.release());
    });
}

void WebRTCManager::SetApmProcessor(std::shared_ptr<AudioApmProcessor> processor) {
    std::lock_guard<std::mutex> lock(apm_mutex_);
    apm_processor_ = std::move(processor);
}

std::shared_ptr<AudioApmProcessor> WebRTCManager::apm_processor() const {
    std::lock_guard<std::mutex> lock(apm_mutex_);
    return apm_processor_;
}

void WebRTCManager::ResetApmProcessor() {
    auto apm = apm_processor();
    if (apm) {
        apm->Reset();
    }
}

} // namespace livekit
