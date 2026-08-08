#pragma once

#include <cstdint>
#include <vector>
#include <cstddef>
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include "dshow_types.h"

namespace livekit {

class MediaConverters {
public:
    // ==========================================
    // 音频转换工具 (WASAPI -> 16bit PCM 48kHz)
    // ==========================================

    // 将 Float32 样本转换为 Int16 样本 (限制范围 -32768 ~ 32767)
    static void ConvertFloatToPcm16(const float* src, int16_t* dst, size_t total_samples);

    // 将 24-bit PCM (3 bytes per sample) 转换为 Int16
    static void ConvertPcm24ToPcm16(const uint8_t* src, int16_t* dst, size_t total_samples);

    // 将 32-bit PCM (4 bytes int) 转换为 Int16
    static void ConvertPcm32ToPcm16(const int32_t* src, int16_t* dst, size_t total_samples);

    // 通用 WASAPI 缓冲区下混与重采样至标准 16-bit PCM (target_channels: 1 or 2, target_rate: 48000)
    static void ProcessWasapiAudioBuffer(
        const uint8_t* in_data,
        uint32_t in_frames,
        const WAVEFORMATEX* wfex,
        int target_rate,
        int target_channels,
        std::vector<int16_t>& out_pcm);

    // ==========================================
    // 视频色彩空间转换 (DirectShow -> RGBA / NV12)
    // ==========================================

    // YUY2 (YUV 4:2:2 Packed) 转换为 32-bit RGBA
    static void ConvertYUY2ToRGBA(const uint8_t* yuy2, uint8_t* rgba, int width, int height, bool flip_v = false);

    // YUY2 (YUV 4:2:2 Packed) 转换为 NV12 (Y plane + interleaved UV plane)
    static void ConvertYUY2ToNV12(const uint8_t* yuy2, uint8_t* nv12, int width, int height);

    // RGB24 (BGR 24-bit) 转换为 32-bit RGBA (支持 BMP 自下而上垂直翻转)
    static void ConvertRGB24ToRGBA(const uint8_t* rgb24, uint8_t* rgba, int width, int height, bool flip_v = false);

    // ARGB32 / BGRA32 转换为 32-bit RGBA (处理通道重排)
    static void ConvertARGB32ToRGBA(const uint8_t* bgra, uint8_t* rgba, int width, int height, bool flip_v = false);

    // NV12 转换为 32-bit RGBA
    static void ConvertNV12ToRGBA(const uint8_t* nv12, uint8_t* rgba, int width, int height, bool flip_v = false);

    // 格式快速名称与 GUID 转换
    static DShowPixelFormat SubtypeToPixelFormat(const GUID& subtype);
    static GUID PixelFormatToSubtype(DShowPixelFormat format);
    static const char* PixelFormatToString(DShowPixelFormat format);

private:
    static inline uint8_t Clamp8(int val) {
        return static_cast<uint8_t>(val < 0 ? 0 : (val > 255 ? 255 : val));
    }

    static inline void YUVToRGB(int y, int u, int v, uint8_t& r, uint8_t& g, uint8_t& b) {
        int c = y - 16;
        int d = u - 128;
        int e = v - 128;
        r = Clamp8((298 * c + 409 * e + 128) >> 8);
        g = Clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
        b = Clamp8((298 * c + 516 * d + 128) >> 8);
    }
};

} // namespace livekit
