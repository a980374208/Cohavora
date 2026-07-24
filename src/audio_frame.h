#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

namespace livekit {

class AudioFrame {
public:
    AudioFrame();
    AudioFrame(std::vector<std::int16_t> data, int sample_rate, int num_channels, int samples_per_channel);
    virtual ~AudioFrame() = default;

    static AudioFrame create(int sample_rate, int num_channels, int samples_per_channel);

    const std::vector<std::int16_t>& data() const noexcept { return data_; }
    std::vector<std::int16_t>& data() noexcept { return data_; }

    std::size_t totalSamples() const noexcept { return data_.size(); }
    int sampleRate() const noexcept { return sample_rate_; }
    int numChannels() const noexcept { return num_channels_; }
    int samplesPerChannel() const noexcept { return samples_per_channel_; }

    double duration() const noexcept;
    std::string toString() const;

private:
    std::vector<std::int16_t> data_;
    int sample_rate_{0};
    int num_channels_{0};
    int samples_per_channel_{0};
};

} // namespace livekit
