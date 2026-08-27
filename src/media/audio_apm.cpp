#include "audio_apm.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment_factory.h"
#include <iostream>
#include <algorithm>

namespace livekit {

std::shared_ptr<AudioApmProcessor> AudioApmProcessor::Create(const ApmConfig& config) {
    return std::make_shared<AudioApmProcessor>(config);
}

AudioApmProcessor::AudioApmProcessor(const ApmConfig& config) {
    apm_ = webrtc::BuiltinAudioProcessingBuilder().Build(webrtc::CreateEnvironment());
    ApplyConfig(config);
}

AudioApmProcessor::~AudioApmProcessor() = default;

void AudioApmProcessor::ApplyConfig(const ApmConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    if (!apm_) return;

    webrtc::AudioProcessing::Config apm_cfg;

    // 1. AEC (Acoustic Echo Cancellation) 回声消除
    apm_cfg.echo_canceller.enabled = config.enable_aec;
    apm_cfg.echo_canceller.mobile_mode = false;

    // 2. ANS (Audio Noise Suppression) 噪声抑制
    apm_cfg.noise_suppression.enabled = config.enable_ans;
    switch (config.ns_level) {
        case NoiseSuppressionLevel::Low:
            apm_cfg.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kLow;
            break;
        case NoiseSuppressionLevel::Moderate:
            apm_cfg.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kModerate;
            break;
        case NoiseSuppressionLevel::High:
            apm_cfg.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
            break;
        case NoiseSuppressionLevel::VeryHigh:
            apm_cfg.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kVeryHigh;
            break;
    }

    // 3. AGC (Automatic Gain Control) 自动增益控制
    apm_cfg.gain_controller1.enabled = config.enable_agc;
    switch (config.agc_mode) {
        case GainControlMode::AdaptiveAnalog:
            apm_cfg.gain_controller1.mode = webrtc::AudioProcessing::Config::GainController1::kAdaptiveAnalog;
            break;
        case GainControlMode::AdaptiveDigital:
            apm_cfg.gain_controller1.mode = webrtc::AudioProcessing::Config::GainController1::kAdaptiveDigital;
            break;
        case GainControlMode::FixedDigital:
            apm_cfg.gain_controller1.mode = webrtc::AudioProcessing::Config::GainController1::kFixedDigital;
            break;
    }
    apm_cfg.gain_controller1.target_level_dbfs = config.target_gain_dbfs;
    apm_cfg.gain_controller1.compression_gain_db = config.compression_gain_db;

    // 4. HPF (High Pass Filter) 高通滤波器 (过滤 80Hz 以下杂音)
    apm_cfg.high_pass_filter.enabled = config.enable_hpf;

    apm_->ApplyConfig(apm_cfg);
    std::cout << "[APM 3A] Applied Config: AEC=" << (config.enable_aec ? "ON" : "OFF")
              << ", ANS=" << (config.enable_ans ? "ON" : "OFF")
              << ", AGC=" << (config.enable_agc ? "ON" : "OFF")
              << ", HPF=" << (config.enable_hpf ? "ON" : "OFF") << std::endl;
}

ApmConfig AudioApmProcessor::GetConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

void AudioApmProcessor::ProcessRenderFrame(const AudioFrame& render_frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!apm_ || !config_.enable_aec || render_frame.data().empty()) return;

    int rate = render_frame.sampleRate();
    int channels = render_frame.numChannels();
    int samples_per_channel = render_frame.samplesPerChannel();

    // APM 严格要求 10ms 帧长 (如 48kHz 下为 480 采样点)
    if (rate <= 0 || channels <= 0 || samples_per_channel != (rate / 100)) {
        return;
    }
    if (render_frame.data().size() < static_cast<size_t>(samples_per_channel * channels)) {
        return;
    }

    webrtc::StreamConfig stream_cfg(rate, channels);
    const int16_t* src_pcm = render_frame.data().data();

    // 10ms 帧长直投 APM 反向参考流 (AEC 扬声器信号)
    apm_->ProcessReverseStream(src_pcm, stream_cfg, stream_cfg, const_cast<int16_t*>(src_pcm));
}

AudioFrame AudioApmProcessor::ProcessCaptureFrame(const AudioFrame& capture_frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!apm_ || capture_frame.data().empty()) {
        return capture_frame;
    }

    int rate = capture_frame.sampleRate();
    int channels = capture_frame.numChannels();
    int samples_per_channel = capture_frame.samplesPerChannel();

    // APM 严格要求 10ms 帧长 (如 48kHz 下为 480 采样点)，若不是 10ms 则跳过 APM 避免越界写
    if (rate <= 0 || channels <= 0 || samples_per_channel != (rate / 100)) {
        return capture_frame;
    }
    if (capture_frame.data().size() < static_cast<size_t>(samples_per_channel * channels)) {
        return capture_frame;
    }

    webrtc::StreamConfig stream_cfg(rate, channels);

    std::vector<int16_t> processed_pcm = capture_frame.data();
    int ret = apm_->ProcessStream(processed_pcm.data(), stream_cfg, stream_cfg, processed_pcm.data());
    if (ret != webrtc::AudioProcessing::kNoError) {
        return capture_frame;
    }

    return AudioFrame(std::move(processed_pcm), rate, channels, samples_per_channel);
}

} // namespace livekit
