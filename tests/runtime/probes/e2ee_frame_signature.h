#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

// Test-only, ephemeral 8x8 luma summary. Never persist these values or images.
// Compare source and decoded content independently of RTP/crypto counters.
namespace e2ee_frame_test {
using Signature = std::array<int, 64>;
template <typename ReadLuma>
std::optional<Signature> Sample(int width, int height, ReadLuma read) {
    if (width < 64 || height < 64) return std::nullopt;
    Signature result{};
    for (int row = 0; row < 8; ++row) for (int col = 0; col < 8; ++col) {
        int total = 0;
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x)
            total += read(((col * 8 + x * 2 + 1) * width) / 64,
                          ((row * 8 + y * 2 + 1) * height) / 64);
        result[row * 8 + col] = (total + 8) / 16;
    }
    return result;
}
inline std::optional<Signature> SampleLuma(const uint8_t* data, int stride, int width, int height) {
    if (!data || stride < width) return std::nullopt;
    return Sample(width, height, [&](int x, int y) { return int(data[y * stride + x]); });
}
// Fixed before runtime: reject flat inputs; tolerate at most 8/255 mean luma
// error from the codec. This is content correspondence, not a quality score.
inline bool HasContrast(const Signature& value) {
    const auto range = std::minmax_element(value.begin(), value.end());
    return *range.second - *range.first >= 16;
}
inline double MeanError(const Signature& a, const Signature& b) {
    double sum = 0;
    for (size_t i = 0; i < a.size(); ++i) sum += std::abs(a[i] - b[i]);
    return sum / a.size();
}
inline bool Matches(const Signature& a, const Signature& b) {
    return HasContrast(a) && HasContrast(b) && MeanError(a, b) <= 8.0;
}
} // namespace e2ee_frame_test
