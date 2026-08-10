#pragma once

#include <memory>
#include <vector>
#include <mutex>
#include <cstdint>
#include "audio_frame.h"
#include "api/scoped_refptr.h"
#include "api/audio/audio_processing.h"

namespace livekit {

enum class NoiseSuppressionLevel {
    Low,
    Moderate,
    High,
    VeryHigh
};

enum class GainControlMode {
    AdaptiveAnalog,
    AdaptiveDigital,
    FixedDigital
};

struct ApmConfig {
    bool enable_aec = true; // AEC: 回声消除
    bool enable_ans = true; // ANS: 噪声抑制
    bool enable_agc = true; // AGC: 自动增益
    bool enable_hpf = true; // HPF: 高通滤波
    NoiseSuppressionLevel ns_level = NoiseSuppressionLevel::High;
    GainControlMode agc_mode = GainControlMode::AdaptiveDigital;
    int target_gain_dbfs = 3;   // AGC 目标增益 (dBFS)
    int compression_gain_db = 9; // AGC 压缩增益 (dB)
};

class AudioApmProcessor {
public:
    static std::shared_ptr<AudioApmProcessor> Create(const ApmConfig& config = {});

    explicit AudioApmProcessor(const ApmConfig& config = {});
    ~AudioApmProcessor();

    void ApplyConfig(const ApmConfig& config);
    ApmConfig GetConfig() const;

    // 处理 10ms 麦克风采集音频帧 (AEC/ANS/AGC 3A 过滤)
    AudioFrame ProcessCaptureFrame(const AudioFrame& capture_frame);

    // 传入扬声器播放音频帧 (为 AEC 回声消除提供参考信号)
    void ProcessRenderFrame(const AudioFrame& render_frame);

private:
    webrtc::scoped_refptr<webrtc::AudioProcessing> apm_;
    ApmConfig config_;
    mutable std::mutex mutex_;

    // 内部 10ms 帧划分与声道解分流缓冲区
    std::vector<int16_t> render_buffer_;
    std::vector<int16_t> capture_buffer_;
};

} // namespace livekit
