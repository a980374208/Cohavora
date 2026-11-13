#include "h265_video_decoder.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// This TU must see the independently packaged FFmpeg 61/59 headers first.
// Every FFmpeg call is dynamically bound, so WebRTC's static FFmpeg cannot
// supply a symbol with an incompatible structure layout or allocator.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/version.h>
}

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/h265_profile_tier_level.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"
#include "common_video/h265/h265_sps_parser.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"

static_assert(LIBAVCODEC_VERSION_MAJOR == 61,
              "Cohavora HEVC decoder requires pinned avcodec-61 SDK headers");
static_assert(LIBAVUTIL_VERSION_MAJOR == 59,
              "Cohavora HEVC decoder requires pinned avutil-59 SDK headers");

namespace livekit {
namespace {
constexpr size_t kMaximumAccessUnitBytes = 8 * 1024 * 1024;
constexpr size_t kMaximumParameterSetBytes = 64 * 1024;
constexpr size_t kMaximumNalus = 4096;
constexpr size_t kMaximumPendingFrames = 32;
constexpr size_t kMaximumPacketInfos = 8192;
constexpr int kMaximumDimension = 4096;
constexpr int64_t kMaximumPixels = int64_t{4096} * 2304;
constexpr uint32_t kMaximumDecodedPictureBuffer = 16;

bool ValidGeometry(int width, int height) {
    return width >= 16 && height >= 16 && width <= kMaximumDimension &&
           height <= kMaximumDimension && width % 2 == 0 && height % 2 == 0 &&
           int64_t{width} * height <= kMaximumPixels;
}

AVPixelFormat SelectSoftware420(AVCodecContext*, const AVPixelFormat* formats) {
    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format)
        if (*format == AV_PIX_FMT_YUV420P) return *format;
    return AV_PIX_FMT_NONE;
}

class FfmpegApi {
public:
    ~FfmpegApi() {
        if (codec_module_) FreeLibrary(codec_module_);
        if (util_module_) FreeLibrary(util_module_);
    }

    bool Load(std::string* error) {
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                               static_cast<DWORD>(executable.size()));
        if (!length || length >= executable.size()) {
            *error = "Cannot resolve the executable directory for the HEVC runtime";
            return false;
        }
        executable.resize(length);
        const auto directory = std::filesystem::path(executable).parent_path() / L"media-codecs";
        // Absolute module paths and local/System32 dependency resolution only.
        // Do not change PATH, the working directory or process DLL search state.
        const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
        util_module_ = LoadLibraryExW((directory / L"avutil-59.dll").c_str(), nullptr, flags);
        if (!util_module_) {
            *error = "Cannot load packaged avutil-59.dll (Win32 " + std::to_string(GetLastError()) + ")";
            return false;
        }
        codec_module_ = LoadLibraryExW((directory / L"avcodec-61.dll").c_str(), nullptr, flags);
        if (!codec_module_) {
            *error = "Cannot load packaged avcodec-61.dll (Win32 " + std::to_string(GetLastError()) + ")";
            return false;
        }
        if (!MatchesPath(util_module_, directory / L"avutil-59.dll") ||
            !MatchesPath(codec_module_, directory / L"avcodec-61.dll")) {
            *error = "HEVC runtime module resolved outside its packaged directory";
            return false;
        }
#define BIND_CODEC(name) if (!Bind(codec_module_, #name, &name, error)) return false
#define BIND_UTIL(name) if (!Bind(util_module_, #name, &name, error)) return false
        BIND_CODEC(avcodec_version);
        BIND_CODEC(avcodec_find_decoder_by_name);
        BIND_CODEC(avcodec_alloc_context3);
        BIND_CODEC(avcodec_open2);
        BIND_CODEC(avcodec_free_context);
        BIND_CODEC(avcodec_flush_buffers);
        BIND_CODEC(avcodec_send_packet);
        BIND_CODEC(avcodec_receive_frame);
        BIND_CODEC(av_packet_alloc);
        BIND_CODEC(av_packet_free);
        BIND_CODEC(av_packet_unref);
        BIND_CODEC(av_new_packet);
        BIND_UTIL(avutil_version);
        BIND_UTIL(av_frame_alloc);
        BIND_UTIL(av_frame_free);
        BIND_UTIL(av_frame_unref);
        BIND_UTIL(av_strerror);
#undef BIND_CODEC
#undef BIND_UTIL
        if (avcodec_version() != LIBAVCODEC_VERSION_INT ||
            avutil_version() != LIBAVUTIL_VERSION_INT) {
            *error = "Packaged FFmpeg versions do not match the compiled HEVC SDK";
            return false;
        }
        return true;
    }

    std::string Describe(int code) const {
        char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
        if (!av_strerror || av_strerror(code, buffer, sizeof(buffer)) < 0)
            return std::to_string(code);
        return std::string(buffer) + " (" + std::to_string(code) + ")";
    }

#define DECLARE_API(name) decltype(&::name) name = nullptr
    DECLARE_API(avcodec_version);
    DECLARE_API(avcodec_find_decoder_by_name);
    DECLARE_API(avcodec_alloc_context3);
    DECLARE_API(avcodec_open2);
    DECLARE_API(avcodec_free_context);
    DECLARE_API(avcodec_flush_buffers);
    DECLARE_API(avcodec_send_packet);
    DECLARE_API(avcodec_receive_frame);
    DECLARE_API(av_packet_alloc);
    DECLARE_API(av_packet_free);
    DECLARE_API(av_packet_unref);
    DECLARE_API(av_new_packet);
    DECLARE_API(avutil_version);
    DECLARE_API(av_frame_alloc);
    DECLARE_API(av_frame_free);
    DECLARE_API(av_frame_unref);
    DECLARE_API(av_strerror);
#undef DECLARE_API

private:
    template<typename Function>
    static bool Bind(HMODULE module, const char* name, Function* target, std::string* error) {
        *target = reinterpret_cast<Function>(GetProcAddress(module, name));
        if (!*target) *error = "Missing packaged FFmpeg export: " + std::string(name);
        return *target != nullptr;
    }

    static bool MatchesPath(HMODULE module, const std::filesystem::path& expected) {
        std::wstring actual(32768, L'\0');
        const DWORD length = GetModuleFileNameW(module, actual.data(), static_cast<DWORD>(actual.size()));
        if (!length || length >= actual.size()) return false;
        actual.resize(length);
        std::error_code error;
        return std::filesystem::equivalent(std::filesystem::path(actual), expected, error) && !error;
    }

    HMODULE codec_module_ = nullptr;
    HMODULE util_module_ = nullptr;
};

void SetContextLimits(AVCodecContext* context, int cores) {
    // Slice workers do not introduce a frame-thread queue. Work, DPB allocation
    // and access units have independent upper bounds.
    context->thread_count = std::clamp(cores, 1, 4);
    context->thread_type = FF_THREAD_SLICE;
    context->pkt_timebase = AVRational{1, 90000};
    context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context->get_format = SelectSoftware420;
    // WebRTC seeds max_render_resolution from the first received frame without
    // reconfiguring this decoder on later size changes. Keep the allocation
    // ceiling independent of that startup hint; each SPS is still checked
    // against the fixed per-axis, pixel and DPB budgets before submission.
    context->max_pixels = kMaximumPixels;
    context->err_recognition = AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE | AV_EF_CAREFUL;
    context->error_concealment = 0;
}

struct Runtime {
    std::shared_ptr<const FfmpegApi> api;
    const AVCodec* codec = nullptr;
    std::string error;

    Runtime() {
        auto candidate = std::make_shared<FfmpegApi>();
        if (!candidate->Load(&error)) return;
        const AVCodec* decoder = candidate->avcodec_find_decoder_by_name("hevc");
        if (!decoder || decoder->id != AV_CODEC_ID_HEVC || decoder->type != AVMEDIA_TYPE_VIDEO) {
            error = "Packaged FFmpeg has no software HEVC decoder";
            return;
        }
        AVCodecContext* probe = candidate->avcodec_alloc_context3(decoder);
        if (!probe) {
            error = "Cannot allocate the HEVC decoder capability probe";
            return;
        }
        SetContextLimits(probe, 1);
        const int opened = candidate->avcodec_open2(probe, decoder, nullptr);
        candidate->avcodec_free_context(&probe);
        if (opened < 0) {
            error = "HEVC decoder capability open failed: " + candidate->Describe(opened);
            return;
        }
        codec = decoder;
        api = std::move(candidate);
    }
};

const Runtime& DecoderRuntime() {
    // Thread-safe initialization; no decoder sessions or mutable codec contexts
    // are shared. Missing/bad packages fail once rather than being reloaded by
    // each SDP/capability query from the UI or WebRTC threads.
    static const Runtime runtime;
    return runtime;
}

bool SupportedFormat(const webrtc::SdpVideoFormat& format) {
    if (_stricmp(format.name.c_str(), "H265") != 0) return false;
    const auto profile = webrtc::ParseSdpForH265ProfileTierLevel(format.parameters);
    if (!profile || profile->profile != webrtc::H265Profile::kProfileMain ||
        profile->tier != webrtc::H265Tier::kTier0 ||
        profile->level > webrtc::H265Level::kLevel5_1) return false;
    const auto mode = format.parameters.find("tx-mode");
    if (mode != format.parameters.end() && _stricmp(mode->second.c_str(), "SRST") != 0) return false;
    const auto don = format.parameters.find("sprop-max-don-diff");
    if (don != format.parameters.end() && don->second != "0") return false;
    for (const char* parameter : {"sprop-depack-buf-nalus", "sprop-depack-buf-bytes"}) {
        const auto value = format.parameters.find(parameter);
        if (value != format.parameters.end() && value->second != "0") return false;
    }
    return true;
}

size_t StartCodeLength(const uint8_t* data, size_t size, size_t offset) {
    if (size - offset < 3 || data[offset] != 0 || data[offset + 1] != 0) return 0;
    if (data[offset + 2] == 1) return 3;
    return size - offset >= 4 && data[offset + 2] == 0 && data[offset + 3] == 1 ? 4 : 0;
}

class H265VideoDecoder final : public webrtc::VideoDecoder {
public:
    explicit H265VideoDecoder(const Runtime& runtime) : api_(runtime.api), codec_(runtime.codec) {}
    ~H265VideoDecoder() override { Reset(); }

    bool Configure(const Settings& settings) override {
        if (!OnSequence() || decoding_) return false;
        Reset();
        error_.clear();
        if (settings.codec_type() != webrtc::kVideoCodecH265 || settings.number_of_cores() <= 0)
            return ConfigurationError("HEVC Configure requires its codec and a positive core count");
        const auto resolution = settings.max_render_resolution();
        // VCMDecoderDatabase supplies the first frame's geometry here, rather
        // than the largest later frame of the same payload type. Validate that
        // hint, but allow a later IRAP/SPS to change size within the fixed budget.
        if (resolution.Valid() && !ValidGeometry(resolution.Width(), resolution.Height()))
            return ConfigurationError("HEVC initial render hint exceeds Main 8-bit 4K budget");
        context_ = api_->avcodec_alloc_context3(codec_);
        frame_ = api_->av_frame_alloc();
        packet_ = api_->av_packet_alloc();
        if (!context_ || !frame_ || !packet_)
            return ConfigurationError("HEVC decoder resource allocation failed");
        SetContextLimits(context_, settings.number_of_cores());
        const int opened = api_->avcodec_open2(context_, codec_, nullptr);
        if (opened < 0) return ConfigurationError("HEVC avcodec_open2: " + api_->Describe(opened));
        configured_ = true;
        needs_keyframe_ = true;
        return true;
    }

    int32_t Decode(const webrtc::EncodedImage& image, int64_t render_time_ms) override {
        (void)render_time_ms;
        if (!OnSequence() || decoding_) return WEBRTC_VIDEO_CODEC_ERROR;
        if (!configured_ || !callback_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
        struct DecodeGuard {
            bool& flag;
            explicit DecodeGuard(bool& value) : flag(value) { flag = true; }
            ~DecodeGuard() { flag = false; }
        } guard(decoding_);
        bool keyframe = false;
        if (!ValidateAccessUnit(image, &keyframe)) return Recover("Invalid or unsupported HEVC access unit");
        if (image.PacketInfos().size() > kMaximumPacketInfos)
            return Recover("HEVC packet metadata exceeds its bound");
        if (image._encodedWidth > kMaximumDimension || image._encodedHeight > kMaximumDimension ||
            (image._encodedWidth && image._encodedHeight &&
             int64_t{image._encodedWidth} * image._encodedHeight > kMaximumPixels))
            return Recover("HEVC encoded geometry exceeds the 4K budget");
        if (!keyframe && decoded_width_ &&
            ((image._encodedWidth && image._encodedWidth != static_cast<uint32_t>(decoded_width_)) ||
             (image._encodedHeight && image._encodedHeight != static_cast<uint32_t>(decoded_height_))))
            return Recover("HEVC geometry change requires a keyframe");
        if (needs_keyframe_ && !keyframe) return WEBRTC_VIDEO_CODEC_OK_REQUEST_KEYFRAME;
        if (pending_.size() >= kMaximumPendingFrames || next_pts_ == std::numeric_limits<int64_t>::max()) {
            Recover("HEVC pending-frame or PTS bound reached");
            if (!keyframe) return WEBRTC_VIDEO_CODEC_OK_REQUEST_KEYFRAME;
        }
        for (const auto& entry : pending_)
            if (entry.second.rtp == image.RtpTimestamp())
                return Recover("Duplicate pending HEVC RTP timestamp");
        api_->av_packet_unref(packet_);
        const int allocated = api_->av_new_packet(packet_, static_cast<int>(image.size()));
        if (allocated < 0) return Recover("HEVC av_new_packet: " + api_->Describe(allocated));
        // av_new_packet owns the bytes and zeroes AV_INPUT_BUFFER_PADDING_SIZE.
        std::memcpy(packet_->data, image.data(), image.size());
        const int64_t pts = next_pts_++;
        packet_->pts = pts;
        packet_->dts = pts;
        if (keyframe) packet_->flags |= AV_PKT_FLAG_KEY;
        Metadata metadata;
        metadata.rtp = image.RtpTimestamp();
        metadata.ntp_ms = image.NtpTimeMs();
        metadata.width = image._encodedWidth;
        metadata.height = image._encodedHeight;
        metadata.keyframe = keyframe;
        metadata.rotation = image.rotation();
        metadata.packet_infos = image.PacketInfos();
        metadata.presentation = image.PresentationTimestamp();
        if (const auto* color = image.ColorSpace()) metadata.color_space = *color;
        int delivered = 0;
        int sent = api_->avcodec_send_packet(context_, packet_);
        if (sent == AVERROR(EAGAIN)) {
            if (!Receive(&delivered)) return WEBRTC_VIDEO_CODEC_ERROR;
            sent = api_->avcodec_send_packet(context_, packet_);
        }
        api_->av_packet_unref(packet_);
        if (sent < 0) return Recover("HEVC avcodec_send_packet: " + api_->Describe(sent));
        pending_.emplace(pts, std::move(metadata));
        needs_keyframe_ = false;
        if (!Receive(&delivered)) return WEBRTC_VIDEO_CODEC_ERROR;
        return delivered ? WEBRTC_VIDEO_CODEC_OK : WEBRTC_VIDEO_CODEC_NO_OUTPUT;
    }

    int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) override {
        if (!OnSequence() || decoding_) return WEBRTC_VIDEO_CODEC_ERROR;
        callback_ = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override {
        if (!OnSequence() || decoding_) return WEBRTC_VIDEO_CODEC_ERROR;
        // A stop never emits delayed callbacks. The caller owns the callback,
        // and may destroy it as soon as Release returns.
        callback_ = nullptr;
        Reset();
        owner_.store(0, std::memory_order_release);
        return WEBRTC_VIDEO_CODEC_OK;
    }

    DecoderInfo GetDecoderInfo() const override {
        DecoderInfo info;
        info.implementation_name = "Cohavora HEVC Main software / avcodec "
            AV_STRINGIFY(LIBAVCODEC_VERSION_MAJOR) "." AV_STRINGIFY(LIBAVCODEC_VERSION_MINOR) "."
            AV_STRINGIFY(LIBAVCODEC_VERSION_MICRO);
        info.is_hardware_accelerated = false;
        return info;
    }

private:
    struct Metadata {
        uint32_t rtp = 0, width = 0, height = 0;
        bool keyframe = false;
        int64_t ntp_ms = 0;
        webrtc::VideoRotation rotation = webrtc::kVideoRotation_0;
        webrtc::RtpPacketInfos packet_infos;
        std::optional<webrtc::Timestamp> presentation;
        std::optional<webrtc::ColorSpace> color_space;
    };

    bool OnSequence() {
        const DWORD current = GetCurrentThreadId();
        DWORD expected = 0;
        if (owner_.compare_exchange_strong(expected, current, std::memory_order_acq_rel) || expected == current)
            return true;
        RTC_LOG(LS_ERROR) << "HEVC decoder called outside its owner sequence";
        return false;
    }

    bool ConfigurationError(std::string message) {
        error_ = std::move(message);
        RTC_LOG(LS_ERROR) << error_;
        Reset();
        return false;
    }

    int32_t Recover(std::string message) {
        if (error_ != message) RTC_LOG(LS_WARNING) << message << "; requesting a new HEVC keyframe";
        error_ = std::move(message);
        if (context_) api_->avcodec_flush_buffers(context_);
        if (packet_) api_->av_packet_unref(packet_);
        if (frame_) api_->av_frame_unref(frame_);
        pending_.clear();
        needs_keyframe_ = true;
        next_pts_ = 1;
        return WEBRTC_VIDEO_CODEC_ERROR;
    }

    void Reset() {
        if (context_) api_->avcodec_free_context(&context_);
        if (packet_) api_->av_packet_free(&packet_);
        if (frame_) api_->av_frame_free(&frame_);
        pending_.clear();
        configured_ = false;
        needs_keyframe_ = true;
        decoded_width_ = decoded_height_ = 0;
        next_pts_ = 1;
    }

    bool ValidateAccessUnit(const webrtc::EncodedImage& image, bool* keyframe) const {
        if (!image.data() || image.size() < 6 || image.size() > kMaximumAccessUnitBytes ||
            image.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return false;
        const uint8_t* data = image.data();
        const size_t size = image.size();
        size_t position = 0, count = 0;
        bool picture = false, geometry_change = false;
        // One pass over a bounded AU, without allocating a vector proportional
        // to attacker-controlled NAL counts. RTP AP/FU/PACI are not Annex-B AUs.
        while (position < size) {
            const size_t prefix = StartCodeLength(data, size, position);
            if (!prefix || ++count > kMaximumNalus) return false;
            const size_t start = position + prefix;
            size_t end = start;
            while (end < size && !StartCodeLength(data, size, end)) ++end;
            position = end;
            size_t payload_end = end;
            while (payload_end > start + 2 && data[payload_end - 1] == 0) --payload_end;
            if (payload_end - start < 3) return false;
            const int type = (data[start] >> 1) & 0x3f;
            const int layer = ((data[start] & 1) << 5) | (data[start + 1] >> 3);
            if ((data[start] & 0x80) || layer != 0 || !(data[start + 1] & 7) || type > 40) return false;
            if (type <= 31) {
                const bool first_slice = (data[start + 2] & 0x80) != 0;
                if ((!picture && !first_slice) || (picture && first_slice)) return false;
                picture = true;
                if (type >= 16 && type <= 21) *keyframe = true;
            } else if (type >= 32 && type <= 34) {
                if (picture || payload_end - start > kMaximumParameterSetBytes) return false;
                if (type == 33) {
                    const auto sps = webrtc::H265SpsParser::ParseSps(data + start + 2, payload_end - start - 2);
                    if (!sps || sps->chroma_format_idc != 1 || sps->separate_colour_plane_flag ||
                        sps->bit_depth_luma_minus8 != 0 ||
                        !ValidGeometry(static_cast<int>(sps->width), static_cast<int>(sps->height)) ||
                        sps->pic_width_in_luma_samples > kMaximumDimension ||
                        sps->pic_height_in_luma_samples > kMaximumDimension ||
                        int64_t{sps->pic_width_in_luma_samples} * sps->pic_height_in_luma_samples > kMaximumPixels)
                        return false;
                    for (const auto buffering : sps->sps_max_dec_pic_buffering_minus1)
                        if (buffering >= kMaximumDecodedPictureBuffer) return false;
                    if (decoded_width_ &&
                        (sps->width != static_cast<uint32_t>(decoded_width_) ||
                         sps->height != static_cast<uint32_t>(decoded_height_)))
                        geometry_change = true;
                }
            }
        }
        return picture && (!geometry_change || *keyframe);
    }

    bool Receive(int* delivered) {
        for (size_t count = 0; count <= kMaximumPendingFrames; ++count) {
            const int result = api_->avcodec_receive_frame(context_, frame_);
            if (result == AVERROR(EAGAIN)) return true;
            if (result < 0) {
                Recover("HEVC avcodec_receive_frame: " + api_->Describe(result));
                return false;
            }
            if (count == kMaximumPendingFrames) {
                Recover("HEVC receive loop exceeded its frame bound");
                return false;
            }
            const bool valid = Deliver();
            api_->av_frame_unref(frame_);
            if (!valid) return false;
            ++*delivered;
        }
        return false;
    }

    bool Deliver() {
        const auto found = pending_.find(frame_->pts);
        if (frame_->pts == AV_NOPTS_VALUE || found == pending_.end() || !callback_)
            return Recover("HEVC output has no matching submitted access unit") == WEBRTC_VIDEO_CODEC_OK;
        const Metadata metadata = found->second;
        if ((frame_->flags & AV_FRAME_FLAG_CORRUPT) || frame_->decode_error_flags ||
            frame_->format != AV_PIX_FMT_YUV420P || context_->profile != AV_PROFILE_HEVC_MAIN)
            return Recover("HEVC output is corrupt or is not Main 8-bit 4:2:0") == WEBRTC_VIDEO_CODEC_OK;
        if (!ValidGeometry(frame_->width, frame_->height) ||
            (metadata.width && metadata.width != static_cast<uint32_t>(frame_->width)) ||
            (metadata.height && metadata.height != static_cast<uint32_t>(frame_->height)))
            return Recover("HEVC decoded geometry differs from its declared bounds") == WEBRTC_VIDEO_CODEC_OK;
        if (decoded_width_ && !metadata.keyframe &&
            (frame_->width != decoded_width_ || frame_->height != decoded_height_))
            return Recover("HEVC decoded geometry changed without a keyframe") == WEBRTC_VIDEO_CODEC_OK;
        if (!frame_->data[0] || !frame_->data[1] || !frame_->data[2] ||
            frame_->linesize[0] < frame_->width || frame_->linesize[1] < frame_->width / 2 ||
            frame_->linesize[2] < frame_->width / 2)
            return Recover("HEVC decoded planes or strides are invalid") == WEBRTC_VIDEO_CODEC_OK;
        auto owned = webrtc::I420Buffer::Copy(frame_->width, frame_->height,
            frame_->data[0], frame_->linesize[0], frame_->data[1], frame_->linesize[1],
            frame_->data[2], frame_->linesize[2]);
        if (!owned) return Recover("HEVC owned I420 allocation failed") == WEBRTC_VIDEO_CODEC_OK;
        auto output = webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(owned)
            .set_rtp_timestamp(metadata.rtp)
            .set_ntp_time_ms(metadata.ntp_ms)
            .set_presentation_timestamp(metadata.presentation)
            .set_rotation(metadata.rotation)
            .set_color_space(metadata.color_space)
            .set_packet_infos(metadata.packet_infos)
            .build();
        pending_.erase(found);
        decoded_width_ = frame_->width;
        decoded_height_ = frame_->height;
        // Output holds an independent refcounted I420 allocation. Decode and
        // its synchronous callback finish before Release can clear callback_.
        // Delayed output has no honest codec-only elapsed time, so leave it unset.
        callback_->Decoded(output, std::nullopt, std::nullopt);
        return true;
    }

    const std::shared_ptr<const FfmpegApi> api_;
    const AVCodec* const codec_;
    AVCodecContext* context_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    webrtc::DecodedImageCallback* callback_ = nullptr;
    std::atomic<DWORD> owner_{0};
    std::map<int64_t, Metadata> pending_;
    int64_t next_pts_ = 1;
    int decoded_width_ = 0, decoded_height_ = 0;
    bool configured_ = false, needs_keyframe_ = true, decoding_ = false;
    std::string error_;
};
}

bool IsH265DecoderAvailable() {
    return DecoderRuntime().api != nullptr;
}

const char* H265DecoderAvailabilityError() {
    return DecoderRuntime().error.c_str();
}

std::unique_ptr<webrtc::VideoDecoder> CreateH265VideoDecoder(
    const webrtc::Environment& environment, const webrtc::SdpVideoFormat& format) {
    (void)environment;
    const auto& runtime = DecoderRuntime();
    if (!runtime.api || !SupportedFormat(format)) return nullptr;
    return std::make_unique<H265VideoDecoder>(runtime);
}
}
