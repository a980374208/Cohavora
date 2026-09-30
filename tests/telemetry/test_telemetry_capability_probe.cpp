#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "api/environment/environment_factory.h"
#include "api/media_types.h"
#include "api/stats/rtc_stats_report.h"
#include "api/stats/rtcstats_objects.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_bitrate_allocation.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_type.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "src/render/api/render_backend_api.h"
#include "tests/support/test_check.h"
#include "webrtc_manager.h"

namespace {

std::string Upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

bool HasCodec(const webrtc::RtpCapabilities& capabilities, const char* expected) {
    return std::any_of(capabilities.codecs.begin(), capabilities.codecs.end(),
        [expected](const webrtc::RtpCodecCapability& codec) {
            return Upper(codec.name) == expected;
        });
}

void PrintCodecs(const char* label, const webrtc::RtpCapabilities& capabilities) {
    std::cout << label << '=';
    bool first = true;
    for (const auto& codec : capabilities.codecs) {
        if (!codec.IsMediaCodec()) continue;
        if (!first) std::cout << ',';
        first = false;
        std::cout << codec.mime_type();
    }
    std::cout << '\n';
}

std::optional<webrtc::SdpVideoFormat> FindCodecFormat(
    const std::vector<webrtc::SdpVideoFormat>& formats,
    const char* codec_name) {
    const auto found = std::find_if(formats.begin(), formats.end(),
        [codec_name](const webrtc::SdpVideoFormat& format) {
            return Upper(format.name) == codec_name;
        });
    if (found == formats.end()) return std::nullopt;
    return *found;
}

class CodecRoundTripCallback final
    : public webrtc::EncodedImageCallback,
      public webrtc::DecodedImageCallback {
public:
    explicit CodecRoundTripCallback(webrtc::VideoDecoder& decoder)
        : decoder_(decoder) {}

    Result OnEncodedImage(
        const webrtc::EncodedImage& encoded_image,
        const webrtc::CodecSpecificInfo*) override {
        encoded_.store(true, std::memory_order_release);
        if (decoder_.Decode(encoded_image, 0) != WEBRTC_VIDEO_CODEC_OK) {
            decode_failed_.store(true, std::memory_order_release);
            condition_.notify_all();
        }
        return Result(Result::OK, encoded_image.RtpTimestamp());
    }

    int32_t Decoded(webrtc::VideoFrame& frame) override {
        RecordDecodedFrame(frame);
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Decoded(webrtc::VideoFrame& frame, int64_t) override {
        RecordDecodedFrame(frame);
        return WEBRTC_VIDEO_CODEC_OK;
    }

    void Decoded(
        webrtc::VideoFrame& frame,
        std::optional<int32_t>,
        std::optional<uint8_t>) override {
        RecordDecodedFrame(frame);
    }

    bool WaitForDecodedFrame(std::chrono::seconds timeout) {
        std::unique_lock lock(mutex_);
        condition_.wait_for(lock, timeout, [this]() {
            return decoded_ || decode_failed_.load(std::memory_order_acquire);
        });
        return encoded_.load(std::memory_order_acquire) && decoded_ &&
            !decode_failed_.load(std::memory_order_acquire);
    }

    int decoded_width() const { return decoded_width_; }
    int decoded_height() const { return decoded_height_; }

private:
    void RecordDecodedFrame(const webrtc::VideoFrame& frame) {
        {
            std::lock_guard lock(mutex_);
            decoded_width_ = frame.width();
            decoded_height_ = frame.height();
            decoded_ = true;
        }
        condition_.notify_all();
    }

    webrtc::VideoDecoder& decoder_;
    std::atomic<bool> encoded_{false};
    std::atomic<bool> decode_failed_{false};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool decoded_ = false;
    int decoded_width_ = 0;
    int decoded_height_ = 0;
};

webrtc::VideoCodec MakeCodecSettings(webrtc::VideoCodecType codec_type) {
    webrtc::VideoCodec settings;
    settings.codecType = codec_type;
    settings.width = 64;
    settings.height = 64;
    settings.startBitrate = 300;
    settings.maxBitrate = 500;
    settings.minBitrate = 30;
    settings.maxFramerate = 30;
    settings.active = true;
    settings.qpMax = 56;
    settings.numberOfSimulcastStreams = 0;
    settings.SetScalabilityMode(webrtc::ScalabilityMode::kL1T1);
    if (codec_type == webrtc::kVideoCodecVP9) {
        *settings.VP9() = webrtc::VideoEncoder::GetDefaultVp9Settings();
    } else if (codec_type == webrtc::kVideoCodecAV1) {
        settings.AV1()->automatic_resize_on = false;
    }
    return settings;
}

void CodecRoundTripSmoke(const char* codec_name, webrtc::VideoCodecType codec_type) {
    const auto environment = webrtc::CreateEnvironment();
    auto encoder_factory = livekit::CreateVideoEncoderFactory();
    auto decoder_factory = livekit::CreateVideoDecoderFactory();
    const auto encoder_format = FindCodecFormat(encoder_factory->GetSupportedFormats(), codec_name);
    const auto decoder_format = FindCodecFormat(decoder_factory->GetSupportedFormats(), codec_name);
    TEST_CHECK(encoder_format.has_value());
    TEST_CHECK(decoder_format.has_value());

    auto encoder = encoder_factory->Create(environment, *encoder_format);
    auto decoder = decoder_factory->Create(environment, *decoder_format);
    TEST_CHECK(encoder);
    TEST_CHECK(decoder);

    CodecRoundTripCallback callback(*decoder);
    TEST_CHECK(encoder->RegisterEncodeCompleteCallback(&callback) == WEBRTC_VIDEO_CODEC_OK);
    TEST_CHECK(decoder->RegisterDecodeCompleteCallback(&callback) == WEBRTC_VIDEO_CODEC_OK);

    auto settings = MakeCodecSettings(codec_type);
    const webrtc::VideoEncoder::Settings encoder_settings(
        webrtc::VideoEncoder::Capabilities(false), 2, 1200);
    TEST_CHECK(encoder->InitEncode(&settings, encoder_settings) == WEBRTC_VIDEO_CODEC_OK);

    webrtc::VideoDecoder::Settings decoder_settings;
    decoder_settings.set_codec_type(codec_type);
    decoder_settings.set_number_of_cores(2);
    TEST_CHECK(decoder->Configure(decoder_settings));

    webrtc::VideoBitrateAllocation bitrate;
    TEST_CHECK(bitrate.SetBitrate(0, 0, 300000));
    encoder->SetRates(webrtc::VideoEncoder::RateControlParameters(bitrate, 30.0));

    auto buffer = webrtc::I420Buffer::Create(64, 64);
    webrtc::I420Buffer::SetBlack(buffer.get());
    const auto frame = webrtc::VideoFrame::Builder()
        .set_video_frame_buffer(buffer)
        .set_timestamp_us(1000000)
        .set_timestamp_rtp(90000)
        .build();
    const std::vector<webrtc::VideoFrameType> frame_types{
        webrtc::VideoFrameType::kVideoFrameKey};
    TEST_CHECK(encoder->Encode(frame, &frame_types) == WEBRTC_VIDEO_CODEC_OK);
    TEST_CHECK(callback.WaitForDecodedFrame(std::chrono::seconds(10)));
    TEST_CHECK(callback.decoded_width() == 64);
    TEST_CHECK(callback.decoded_height() == 64);

    TEST_CHECK(encoder->Release() == WEBRTC_VIDEO_CODEC_OK);
    TEST_CHECK(decoder->Release() == WEBRTC_VIDEO_CODEC_OK);
    std::cout << "codec_roundtrip_" << Upper(codec_name) << "=PASS\n";
}

void CodecFactoryPolicyProbe() {
    const auto environment = webrtc::CreateEnvironment();
    auto encoder_factory = livekit::CreateVideoEncoderFactory();
    auto decoder_factory = livekit::CreateVideoDecoderFactory();
    const auto vp9 = FindCodecFormat(encoder_factory->GetSupportedFormats(), "VP9");
    const auto av1 = FindCodecFormat(encoder_factory->GetSupportedFormats(), "AV1");
    const auto h265_encoder = FindCodecFormat(
        encoder_factory->GetSupportedFormats(), "H265");
    const auto h265_decoder = FindCodecFormat(
        decoder_factory->GetSupportedFormats(), "H265");
    TEST_CHECK(vp9.has_value());
    TEST_CHECK(av1.has_value());
    TEST_CHECK(!h265_encoder.has_value());
    TEST_CHECK(!h265_decoder.has_value());
    TEST_CHECK(!livekit::IsVideoEncoderFormatSupported("h265"));

    TEST_CHECK(encoder_factory->QueryCodecSupport(*vp9, "L1T1").is_supported);
    TEST_CHECK(!encoder_factory->QueryCodecSupport(*vp9, "invalid-mode").is_supported);
    TEST_CHECK(encoder_factory->QueryCodecSupport(*av1, "L1T1").is_supported);
    TEST_CHECK(!encoder_factory->QueryCodecSupport(*av1, "L2T1").is_supported);

    const auto av1_profile1 = webrtc::SdpVideoFormat::AV1Profile1();
    TEST_CHECK(!encoder_factory->QueryCodecSupport(av1_profile1, std::nullopt).is_supported);
    TEST_CHECK(!encoder_factory->Create(environment, av1_profile1));
    TEST_CHECK(decoder_factory->Create(environment, av1_profile1));
    std::cout << "codec_factory_policy=PASS\n";
}

void NativeStatsAbiProbe() {
    const auto timestamp = webrtc::Timestamp::Millis(4242);
    auto report = webrtc::RTCStatsReport::Create(timestamp);

    auto inbound = std::make_unique<webrtc::RTCInboundRtpStreamStats>("inbound", timestamp);
    inbound->kind = "audio";
    inbound->track_identifier = "remote-audio";
    inbound->mid = "0";
    inbound->bytes_received = 4096;
    inbound->packets_received = 64;
    inbound->packets_lost = -2;
    inbound->jitter = 0.004;
    inbound->jitter_buffer_delay = 1.25;
    inbound->jitter_buffer_target_delay = 1.5;
    inbound->jitter_buffer_minimum_delay = 0.75;
    inbound->jitter_buffer_emitted_count = 100;
    inbound->total_samples_received = 48000;
    inbound->concealed_samples = 480;
    inbound->silent_concealed_samples = 240;
    inbound->concealment_events = 3;
    inbound->inserted_samples_for_deceleration = 24;
    inbound->removed_samples_for_acceleration = 12;
    inbound->total_decode_time = 0.25;
    inbound->freeze_count = 2;
    inbound->total_freezes_duration = 0.6;
    inbound->fir_count = 1;
    inbound->pli_count = 2;
    inbound->nack_count = 4;
    report->AddStats(std::move(inbound));

    auto outbound = std::make_unique<webrtc::RTCOutboundRtpStreamStats>("outbound", timestamp);
    outbound->kind = "video";
    outbound->media_source_id = "source";
    outbound->remote_id = "remote-inbound";
    outbound->mid = "1";
    outbound->rid = "f";
    outbound->bytes_sent = 8192;
    outbound->packets_sent = 80;
    outbound->retransmitted_packets_sent = 5;
    outbound->retransmitted_bytes_sent = 512;
    outbound->frames_encoded = 30;
    outbound->total_encode_time = 0.4;
    outbound->quality_limitation_reason = "bandwidth";
    outbound->quality_limitation_durations = std::map<std::string, double>{{"bandwidth", 1.0}};
    outbound->encoder_implementation = "probe";
    outbound->fir_count = 1;
    outbound->pli_count = 2;
    outbound->nack_count = 3;
    report->AddStats(std::move(outbound));

    auto transport = std::make_unique<webrtc::RTCTransportStats>("transport", timestamp);
    transport->bytes_sent = 9000;
    transport->bytes_received = 7000;
    transport->selected_candidate_pair_id = "pair";
    transport->selected_candidate_pair_changes = 1;
    transport->ice_state = "connected";
    transport->dtls_state = "connected";
    report->AddStats(std::move(transport));

    auto codec = std::make_unique<webrtc::RTCCodecStats>("codec", timestamp);
    codec->transport_id = "transport";
    codec->payload_type = 111;
    codec->mime_type = "audio/opus";
    codec->clock_rate = 48000;
    codec->channels = 2;
    report->AddStats(std::move(codec));

    auto playout = std::make_unique<webrtc::RTCAudioPlayoutStats>("playout", timestamp);
    playout->kind = "audio";
    playout->synthesized_samples_duration = 0.02;
    playout->synthesized_samples_events = 1;
    playout->total_playout_delay = 0.5;
    playout->total_samples_count = 48000;
    report->AddStats(std::move(playout));

    bool saw_inbound = false;
    bool saw_outbound = false;
    bool saw_transport = false;
    bool saw_codec = false;
    bool saw_playout = false;
    for (const auto& stats : *report) {
        if (stats.type() == webrtc::RTCInboundRtpStreamStats::kType) {
            const auto& value = static_cast<const webrtc::RTCInboundRtpStreamStats&>(stats);
            TEST_CHECK(value.packets_lost == -2);
            TEST_CHECK(value.jitter_buffer_emitted_count == 100);
            TEST_CHECK(value.concealed_samples == 480);
            TEST_CHECK(value.freeze_count == 2);
            TEST_CHECK(value.total_decode_time == 0.25);
            saw_inbound = true;
        } else if (stats.type() == webrtc::RTCOutboundRtpStreamStats::kType) {
            const auto& value = static_cast<const webrtc::RTCOutboundRtpStreamStats&>(stats);
            TEST_CHECK(value.retransmitted_packets_sent == 5);
            TEST_CHECK(value.total_encode_time == 0.4);
            TEST_CHECK(value.quality_limitation_reason == "bandwidth");
            saw_outbound = true;
        } else if (stats.type() == webrtc::RTCTransportStats::kType) {
            const auto& value = static_cast<const webrtc::RTCTransportStats&>(stats);
            TEST_CHECK(value.selected_candidate_pair_id == "pair");
            TEST_CHECK(value.selected_candidate_pair_changes == 1);
            saw_transport = true;
        } else if (stats.type() == webrtc::RTCCodecStats::kType) {
            const auto& value = static_cast<const webrtc::RTCCodecStats&>(stats);
            TEST_CHECK(value.mime_type == "audio/opus");
            TEST_CHECK(value.channels == 2);
            saw_codec = true;
        } else if (stats.type() == webrtc::RTCAudioPlayoutStats::kType) {
            const auto& value = static_cast<const webrtc::RTCAudioPlayoutStats&>(stats);
            TEST_CHECK(value.synthesized_samples_events == 1);
            TEST_CHECK(value.total_playout_delay == 0.5);
            saw_playout = true;
        }
    }
    TEST_CHECK(saw_inbound && saw_outbound && saw_transport && saw_codec && saw_playout);
    std::cout << "native_stats_abi=PASS\n";
}

void RenderAbiProbe() {
    static_assert(std::is_standard_layout_v<lk_render_api>);
    static_assert(std::is_standard_layout_v<lk_render_frame_view>);
    static_assert(LK_RENDER_ABI_V1 == 1);
    static_assert(offsetof(lk_render_api, query_extension) +
                      sizeof(((lk_render_api*)nullptr)->query_extension) ==
                  sizeof(lk_render_api));
    std::cout << "render_abi=" << LK_RENDER_ABI_V1 << '\n';
    std::cout << "render_completion_boundary=render_return_only\n";
}

void CodecCapabilityProbe() {
    auto& manager = livekit::WebRTCManager::Instance();
    TEST_CHECK(manager.Initialize());
    {
        const auto factory = manager.factory();
        TEST_CHECK(factory);

        const auto video_send = factory->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO);
        const auto video_receive = factory->GetRtpReceiverCapabilities(webrtc::MediaType::VIDEO);
        const auto audio_send = factory->GetRtpSenderCapabilities(webrtc::MediaType::AUDIO);
        const auto audio_receive = factory->GetRtpReceiverCapabilities(webrtc::MediaType::AUDIO);

        PrintCodecs("video_send_codecs", video_send);
        PrintCodecs("video_receive_codecs", video_receive);
        PrintCodecs("audio_send_codecs", audio_send);
        PrintCodecs("audio_receive_codecs", audio_receive);

        TEST_CHECK(HasCodec(video_send, "VP8"));
        TEST_CHECK(HasCodec(video_send, "H264"));
        TEST_CHECK(HasCodec(video_send, "VP9"));
        TEST_CHECK(HasCodec(video_send, "AV1"));
        TEST_CHECK(HasCodec(video_receive, "VP8"));
        TEST_CHECK(HasCodec(video_receive, "VP9"));
        TEST_CHECK(HasCodec(video_receive, "H264"));
        TEST_CHECK(HasCodec(video_receive, "AV1"));
        TEST_CHECK(!HasCodec(video_send, "H265"));
        TEST_CHECK(!HasCodec(video_receive, "H265"));
        TEST_CHECK(HasCodec(audio_send, "OPUS"));
        TEST_CHECK(HasCodec(audio_receive, "OPUS"));
    }

    CodecFactoryPolicyProbe();
    CodecRoundTripSmoke("VP9", webrtc::kVideoCodecVP9);
    CodecRoundTripSmoke("AV1", webrtc::kVideoCodecAV1);

    manager.Deinitialize();
    std::cout << "codec_capabilities=PASS\n";
}

} // namespace

int main() {
    NativeStatsAbiProbe();
    RenderAbiProbe();
    CodecCapabilityProbe();
    std::cout << "TELEMETRY_S0_CAPABILITY_PROBE PASS\n";
    return 0;
}
