#pragma once

#include "api/audio/audio_device_defines.h"
#include "audio_playout_warmup.h"

namespace livekit::detail {

// Shared by the native ADM and device-free callback contract regressions.
// The inner transport must outlive this wrapper; processing runs on the
// render thread, while ResetWarmup only updates its atomic generation.
class PlayoutAudioTransportWrapper : public webrtc::AudioTransport {
public:
    explicit PlayoutAudioTransportWrapper(webrtc::AudioTransport* inner)
        : inner_(inner) {}

    void ResetWarmup() { warmup_.Reset(); }

    int32_t RecordedDataIsAvailable(const void* audioSamples,
                                   size_t nSamples,
                                   size_t nBytesPerSample,
                                   size_t nChannels,
                                   uint32_t samplesPerSec,
                                   uint32_t totalDelayMS,
                                   int32_t clockDrift,
                                   uint32_t currentMicLevel,
                                   bool keyPressed,
                                   uint32_t& newMicLevel) override;

    int32_t NeedMorePlayData(size_t nSamples,
                            size_t nBytesPerSample,
                            size_t nChannels,
                            uint32_t samplesPerSec,
                            void* audioSamples,
                            size_t& nSamplesOut,
                            int64_t* elapsed_time_ms,
                            int64_t* ntp_time_ms) override;

    void PullRenderData(int bits_per_sample,
                        int sample_rate,
                        size_t number_of_channels,
                        size_t number_of_frames,
                        void* audio_data,
                        int64_t* elapsed_time_ms,
                        int64_t* ntp_time_ms) override;

private:
    webrtc::AudioTransport* inner_;
    AudioPlayoutWarmup warmup_;
};

} // namespace livekit::detail
