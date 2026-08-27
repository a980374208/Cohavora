#include "media_converters.h"
#include <cguid.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace livekit {

// GUIDs for DirectShow Video Subtypes
static const GUID MEDIASUBTYPE_YUY2_LOCAL =
    { 0x32595559, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID MEDIASUBTYPE_NV12_LOCAL =
    { 0x3231564e, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID MEDIASUBTYPE_RGB24_LOCAL =
    { 0xe436eb7d, 0x524f, 0x11ce, { 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
static const GUID MEDIASUBTYPE_RGB32_LOCAL =
    { 0xe436eb7e, 0x524f, 0x11ce, { 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
static const GUID MEDIASUBTYPE_ARGB32_LOCAL =
    { 0x773c9ac0, 0x3274, 0x11d0, { 0xb7, 0x24, 0x00, 0xaa, 0x00, 0x6c, 0x1a, 0x01 } };
static const GUID MEDIASUBTYPE_MJPG_LOCAL =
    { 0x47504a4d, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID MEDIASUBTYPE_UYVY_LOCAL =
    { 0x59565955, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

void MediaConverters::ConvertFloatToPcm16(const float* src, int16_t* dst, size_t total_samples) {
    for (size_t i = 0; i < total_samples; ++i) {
        float val = src[i] * 32767.0f;
        if (val > 32767.0f) val = 32767.0f;
        else if (val < -32768.0f) val = -32768.0f;
        dst[i] = static_cast<int16_t>(std::lrintf(val));
    }
}

void MediaConverters::ConvertPcm24ToPcm16(const uint8_t* src, int16_t* dst, size_t total_samples) {
    for (size_t i = 0; i < total_samples; ++i) {
        // 24-bit little endian: low, mid, high
        int32_t val = (static_cast<int32_t>(src[i * 3 + 2]) << 24) |
                      (static_cast<int32_t>(src[i * 3 + 1]) << 16) |
                      (static_cast<int32_t>(src[i * 3]) << 8);
        dst[i] = static_cast<int16_t>(val >> 16);
    }
}

void MediaConverters::ConvertPcm32ToPcm16(const int32_t* src, int16_t* dst, size_t total_samples) {
    for (size_t i = 0; i < total_samples; ++i) {
        dst[i] = static_cast<int16_t>(src[i] >> 16);
    }
}

void MediaConverters::ProcessWasapiAudioBuffer(
    const uint8_t* in_data,
    uint32_t in_frames,
    const WAVEFORMATEX* wfex,
    int target_rate,
    int target_channels,
    std::vector<int16_t>& out_pcm)
{
    if (!in_data || in_frames == 0 || !wfex) return;

    WORD in_channels = wfex->nChannels;
    DWORD in_rate = wfex->nSamplesPerSec;
    bool is_float = false;

    if (wfex->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        is_float = true;
    } else if (wfex->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfex);
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            is_float = true;
        }
    }

    // Step 1: Decode all channels into intermediate float representation [-1.0f, 1.0f]
    std::vector<float> decoded_channels(in_frames * in_channels);
    if (is_float) {
        const float* fsrc = reinterpret_cast<const float*>(in_data);
        std::copy(fsrc, fsrc + (in_frames * in_channels), decoded_channels.begin());
    } else if (wfex->wBitsPerSample == 16) {
        const int16_t* ssrc = reinterpret_cast<const int16_t*>(in_data);
        for (size_t i = 0; i < in_frames * in_channels; ++i) {
            decoded_channels[i] = ssrc[i] / 32768.0f;
        }
    } else if (wfex->wBitsPerSample == 24) {
        for (size_t i = 0; i < in_frames * in_channels; ++i) {
            int32_t val = (static_cast<int32_t>(in_data[i * 3 + 2]) << 24) |
                          (static_cast<int32_t>(in_data[i * 3 + 1]) << 16) |
                          (static_cast<int32_t>(in_data[i * 3]) << 8);
            decoded_channels[i] = (val >> 8) / 8388608.0f;
        }
    } else if (wfex->wBitsPerSample == 32) {
        const int32_t* isrc = reinterpret_cast<const int32_t*>(in_data);
        for (size_t i = 0; i < in_frames * in_channels; ++i) {
            decoded_channels[i] = isrc[i] / 2147483648.0f;
        }
    } else {
        return;
    }

    // Step 2: Downmix or Upmix to target channels (Stereo or Mono)
    std::vector<float> mixed_channels(in_frames * target_channels);
    if (target_channels == 1) {
        if (in_channels == 1) {
            for (uint32_t f = 0; f < in_frames; ++f) {
                mixed_channels[f] = decoded_channels[f];
            }
        } else if (in_channels == 2) {
            for (uint32_t f = 0; f < in_frames; ++f) {
                mixed_channels[f] = 0.5f * (decoded_channels[f * 2 + 0] + decoded_channels[f * 2 + 1]);
            }
        } else {
            for (uint32_t f = 0; f < in_frames; ++f) {
                float sum = 0.0f;
                for (WORD c = 0; c < in_channels; ++c) {
                    sum += decoded_channels[f * in_channels + c];
                }
                mixed_channels[f] = sum / in_channels;
            }
        }
    } else if (target_channels == 2) {
        if (in_channels == 1) {
            for (uint32_t f = 0; f < in_frames; ++f) {
                float mono = decoded_channels[f];
                mixed_channels[f * 2 + 0] = mono;
                mixed_channels[f * 2 + 1] = mono;
            }
        } else if (in_channels == 2) {
            for (uint32_t f = 0; f < in_frames; ++f) {
                mixed_channels[f * 2 + 0] = decoded_channels[f * 2 + 0];
                mixed_channels[f * 2 + 1] = decoded_channels[f * 2 + 1];
            }
        } else {
            for (uint32_t f = 0; f < in_frames; ++f) {
                float l = decoded_channels[f * in_channels + 0];
                float r = decoded_channels[f * in_channels + 1];
                float c = (in_channels >= 3) ? decoded_channels[f * in_channels + 2] : 0.0f;
                float lfe = (in_channels >= 4) ? decoded_channels[f * in_channels + 3] : 0.0f;
                float ls = (in_channels >= 5) ? decoded_channels[f * in_channels + 4] : 0.0f;
                float rs = (in_channels >= 6) ? decoded_channels[f * in_channels + 5] : 0.0f;

                mixed_channels[f * 2 + 0] = (l + 0.707f * c + 0.707f * ls + 0.5f * lfe) * 0.7f;
                mixed_channels[f * 2 + 1] = (r + 0.707f * c + 0.707f * rs + 0.5f * lfe) * 0.7f;
            }
        }
    } else {
        for (uint32_t f = 0; f < in_frames; ++f) {
            for (int c = 0; c < target_channels; ++c) {
                mixed_channels[f * target_channels + c] = (c < in_channels) ? decoded_channels[f * in_channels + c] : 0.0f;
            }
        }
    }

    // Step 3: Resample to target_rate (Linear interpolation for low latency and high quality)
    uint32_t out_frames = static_cast<uint32_t>(static_cast<uint64_t>(in_frames) * target_rate / in_rate);
    if (out_frames == 0) out_frames = 1;

    out_pcm.resize(out_frames * target_channels);

    double ratio = static_cast<double>(in_frames - 1) / (out_frames > 1 ? (out_frames - 1) : 1);
    for (uint32_t i = 0; i < out_frames; ++i) {
        double src_idx = i * ratio;
        uint32_t idx0 = static_cast<uint32_t>(src_idx);
        uint32_t idx1 = std::min(idx0 + 1, in_frames - 1);
        float frac = static_cast<float>(src_idx - idx0);

        for (int ch = 0; ch < target_channels; ++ch) {
            float s0 = mixed_channels[idx0 * target_channels + ch];
            float s1 = mixed_channels[idx1 * target_channels + ch];
            float interp = s0 + frac * (s1 - s0);

            float val = interp * 32767.0f;
            if (val > 32767.0f) val = 32767.0f;
            else if (val < -32768.0f) val = -32768.0f;
            out_pcm[i * target_channels + ch] = static_cast<int16_t>(std::lrintf(val));
        }
    }
}

void MediaConverters::ConvertYUY2ToRGBA(const uint8_t* yuy2, uint8_t* rgba, int width, int height, bool flip_v) {
    if (!yuy2 || !rgba || width <= 0 || height <= 0) return;

    for (int y = 0; y < height; ++y) {
        int src_y = flip_v ? (height - 1 - y) : y;
        const uint8_t* p_src = yuy2 + (src_y * width * 2);
        uint8_t* p_dst = rgba + (y * width * 4);

        for (int x = 0; x < width; x += 2) {
            int y0 = p_src[0];
            int u  = p_src[1];
            int y1 = p_src[2];
            int v  = p_src[3];
            p_src += 4;

            uint8_t r0, g0, b0;
            YUVToRGB(y0, u, v, r0, g0, b0);
            p_dst[0] = r0;
            p_dst[1] = g0;
            p_dst[2] = b0;
            p_dst[3] = 255;

            uint8_t r1, g1, b1;
            YUVToRGB(y1, u, v, r1, g1, b1);
            p_dst[4] = r1;
            p_dst[5] = g1;
            p_dst[6] = b1;
            p_dst[7] = 255;

            p_dst += 8;
        }
    }
}

void MediaConverters::ConvertYUY2ToNV12(const uint8_t* yuy2, uint8_t* nv12, int width, int height) {
    if (!yuy2 || !nv12 || width <= 0 || height <= 0) return;

    uint8_t* y_plane = nv12;
    uint8_t* uv_plane = nv12 + (width * height);

    for (int y = 0; y < height; ++y) {
        const uint8_t* p_src = yuy2 + (y * width * 2);
        uint8_t* p_y = y_plane + (y * width);
        uint8_t* p_uv = uv_plane + ((y / 2) * width);

        for (int x = 0; x < width; x += 2) {
            p_y[x]     = p_src[0]; // Y0
            p_y[x + 1] = p_src[2]; // Y1

            if ((y % 2) == 0) {
                p_uv[x]     = p_src[1]; // U
                p_uv[x + 1] = p_src[3]; // V
            }
            p_src += 4;
        }
    }
}

void MediaConverters::ConvertRGB24ToRGBA(const uint8_t* rgb24, uint8_t* rgba, int width, int height, bool flip_v) {
    if (!rgb24 || !rgba || width <= 0 || height <= 0) return;

    for (int y = 0; y < height; ++y) {
        int src_y = flip_v ? (height - 1 - y) : y;
        const uint8_t* p_src = rgb24 + (src_y * width * 3);
        uint8_t* p_dst = rgba + (y * width * 4);

        for (int x = 0; x < width; ++x) {
            // DirectShow RGB24 is B, G, R
            p_dst[0] = p_src[2]; // R
            p_dst[1] = p_src[1]; // G
            p_dst[2] = p_src[0]; // B
            p_dst[3] = 255;      // A
            p_src += 3;
            p_dst += 4;
        }
    }
}

void MediaConverters::ConvertARGB32ToRGBA(const uint8_t* bgra, uint8_t* rgba, int width, int height, bool flip_v) {
    if (!bgra || !rgba || width <= 0 || height <= 0) return;

    for (int y = 0; y < height; ++y) {
        int src_y = flip_v ? (height - 1 - y) : y;
        const uint8_t* p_src = bgra + (src_y * width * 4);
        uint8_t* p_dst = rgba + (y * width * 4);

        for (int x = 0; x < width; ++x) {
            // Windows RGB32/ARGB is B, G, R, A
            p_dst[0] = p_src[2]; // R
            p_dst[1] = p_src[1]; // G
            p_dst[2] = p_src[0]; // B
            p_dst[3] = p_src[3]; // A
            p_src += 4;
            p_dst += 4;
        }
    }
}

void MediaConverters::ConvertNV12ToRGBA(const uint8_t* nv12, uint8_t* rgba, int width, int height, bool flip_v) {
    if (!nv12 || !rgba || width <= 0 || height <= 0) return;

    const uint8_t* y_plane = nv12;
    const uint8_t* uv_plane = nv12 + (width * height);

    for (int y = 0; y < height; ++y) {
        int src_y = flip_v ? (height - 1 - y) : y;
        const uint8_t* p_y = y_plane + (src_y * width);
        const uint8_t* p_uv = uv_plane + ((src_y / 2) * width);
        uint8_t* p_dst = rgba + (y * width * 4);

        for (int x = 0; x < width; ++x) {
            int y_val = p_y[x];
            int u_val = p_uv[(x & ~1) + 0];
            int v_val = p_uv[(x & ~1) + 1];

            uint8_t r, g, b;
            YUVToRGB(y_val, u_val, v_val, r, g, b);
            p_dst[0] = r;
            p_dst[1] = g;
            p_dst[2] = b;
            p_dst[3] = 255;
            p_dst += 4;
        }
    }
}

DShowPixelFormat MediaConverters::SubtypeToPixelFormat(const GUID& subtype) {
    if (IsEqualGUID(subtype, MEDIASUBTYPE_YUY2_LOCAL)) return DShowPixelFormat::YUY2;
    if (IsEqualGUID(subtype, MEDIASUBTYPE_NV12_LOCAL)) return DShowPixelFormat::NV12;
    if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB24_LOCAL)) return DShowPixelFormat::RGB24;
    if (IsEqualGUID(subtype, MEDIASUBTYPE_RGB32_LOCAL)) return DShowPixelFormat::ARGB32;
    if (IsEqualGUID(subtype, MEDIASUBTYPE_ARGB32_LOCAL)) return DShowPixelFormat::ARGB32;
    if (IsEqualGUID(subtype, MEDIASUBTYPE_MJPG_LOCAL)) return DShowPixelFormat::MJPEG;
    if (IsEqualGUID(subtype, MEDIASUBTYPE_UYVY_LOCAL)) return DShowPixelFormat::UYVY;
    return DShowPixelFormat::Unknown;
}

GUID MediaConverters::PixelFormatToSubtype(DShowPixelFormat format) {
    switch (format) {
        case DShowPixelFormat::YUY2: return MEDIASUBTYPE_YUY2_LOCAL;
        case DShowPixelFormat::NV12: return MEDIASUBTYPE_NV12_LOCAL;
        case DShowPixelFormat::RGB24: return MEDIASUBTYPE_RGB24_LOCAL;
        case DShowPixelFormat::ARGB32: return MEDIASUBTYPE_ARGB32_LOCAL;
        case DShowPixelFormat::MJPEG: return MEDIASUBTYPE_MJPG_LOCAL;
        case DShowPixelFormat::UYVY: return MEDIASUBTYPE_UYVY_LOCAL;
        default: return GUID_NULL;
    }
}

const char* MediaConverters::PixelFormatToString(DShowPixelFormat format) {
    switch (format) {
        case DShowPixelFormat::YUY2: return "YUY2";
        case DShowPixelFormat::NV12: return "NV12";
        case DShowPixelFormat::RGB24: return "RGB24";
        case DShowPixelFormat::ARGB32: return "ARGB32";
        case DShowPixelFormat::MJPEG: return "MJPEG";
        case DShowPixelFormat::UYVY: return "UYVY";
        default: return "Unknown";
    }
}

} // namespace livekit
