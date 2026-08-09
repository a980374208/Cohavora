#include "audio_frame.h"
#include <sstream>

namespace livekit {

AudioFrame::AudioFrame()
    : sample_rate_(0), num_channels_(0), samples_per_channel_(0) {}

AudioFrame::AudioFrame(std::vector<std::int16_t> data, int sample_rate, int num_channels, int samples_per_channel)
    : data_(std::move(data)), sample_rate_(sample_rate), num_channels_(num_channels), samples_per_channel_(samples_per_channel) {
    if (sample_rate <= 0 || num_channels <= 0 || samples_per_channel < 0) {
        throw std::invalid_argument("Invalid audio frame parameters");
    }
    if (data_.size() != static_cast<std::size_t>(num_channels * samples_per_channel)) {
        throw std::invalid_argument("Data size does not match num_channels * samples_per_channel");
    }
}

AudioFrame AudioFrame::create(int sample_rate, int num_channels, int samples_per_channel) {
    std::size_t total = static_cast<std::size_t>(num_channels * samples_per_channel);
    std::vector<std::int16_t> buffer(total, 0);
    return AudioFrame(std::move(buffer), sample_rate, num_channels, samples_per_channel);
}

double AudioFrame::duration() const noexcept {
    if (sample_rate_ == 0) return 0.0;
    return static_cast<double>(samples_per_channel_) / static_cast<double>(sample_rate_);
}

std::string AudioFrame::toString() const {
    std::stringstream ss;
    ss << "AudioFrame[sample_rate=" << sample_rate_
       << ", channels=" << num_channels_
       << ", samples_per_channel=" << samples_per_channel_
       << ", total_samples=" << data_.size()
       << ", duration=" << duration() << "s]";
    return ss.str();
}

} // namespace livekit
