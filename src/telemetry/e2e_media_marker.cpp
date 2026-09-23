#include "e2e_media_marker.h"

#include "render/owned_i420_frame.h"
#include "rtc/video_frame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace livekit::telemetry {
namespace {

constexpr std::uint8_t kMagic0 = 'C';
constexpr std::uint8_t kMagic1 = 'H';
constexpr std::size_t kPayloadBytes = 16;
constexpr std::size_t kMarkerBytes = 20;
constexpr int kBitsPerRow = 20;
constexpr int kGridRows = 8;
constexpr int kGridColumns = kBitsPerRow * 2;
constexpr std::uint8_t kLowLuma = 16;
constexpr std::uint8_t kHighLuma = 235;
constexpr std::int64_t kMinimumPairDelta = 48;

struct MarkerRegion {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
};

std::optional<MarkerRegion> RegionFor(int width, int height) {
    if (width <= 0 || height <= 0) return std::nullopt;
    MarkerRegion result;
    result.left = width / 16;
    result.top = height / 16;
    result.width = width - 2 * result.left;
    result.height = height / 2;
    if (result.width < kGridColumns * 2 || result.height < kGridRows * 2) {
        return std::nullopt;
    }
    return result;
}

std::uint32_t Crc32(const std::uint8_t* bytes, std::size_t size) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(
                -static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

void Store32(std::uint8_t* destination, std::uint32_t value) {
    for (int i = 3; i >= 0; --i) {
        destination[3 - i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

void Store64(std::uint8_t* destination, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        destination[7 - i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

std::uint32_t Load32(const std::uint8_t* source) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value = (value << 8U) | source[i];
    return value;
}

std::uint64_t Load64(const std::uint8_t* source) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8U) | source[i];
    return value;
}

std::array<std::uint8_t, kMarkerBytes> EncodeMarker(
        const E2eMediaMarker& marker) {
    std::array<std::uint8_t, kMarkerBytes> bytes{};
    bytes[0] = kMagic0;
    bytes[1] = kMagic1;
    bytes[2] = kE2eMediaMarkerVersion;
    bytes[3] = 0;
    Store64(bytes.data() + 4, marker.probe_id);
    Store32(bytes.data() + 12, marker.sequence);
    Store32(bytes.data() + 16, Crc32(bytes.data(), kPayloadBytes));
    return bytes;
}

void FillCell(std::uint8_t* data_y,
              int stride_y,
              const MarkerRegion& region,
              int column,
              int row,
              std::uint8_t value) {
    const int x0 = region.left + column * region.width / kGridColumns;
    const int x1 = region.left + (column + 1) * region.width / kGridColumns;
    const int y0 = region.top + row * region.height / kGridRows;
    const int y1 = region.top + (row + 1) * region.height / kGridRows;
    for (int y = y0; y < y1; ++y) {
        auto* destination = data_y + static_cast<std::ptrdiff_t>(y) * stride_y;
        for (int x = x0; x < x1; ++x) destination[x] = value;
    }
}

std::int64_t CellAverage(const std::uint8_t* data_y,
                         int stride_y,
                         const MarkerRegion& region,
                         int column,
                         int row) {
    const int x0 = region.left + column * region.width / kGridColumns;
    const int x1 = region.left + (column + 1) * region.width / kGridColumns;
    const int y0 = region.top + row * region.height / kGridRows;
    const int y1 = region.top + (row + 1) * region.height / kGridRows;
    std::uint64_t sum = 0;
    std::uint64_t count = 0;
    for (int y = y0; y < y1; ++y) {
        const auto* source = data_y + static_cast<std::ptrdiff_t>(y) * stride_y;
        for (int x = x0; x < x1; ++x) {
            sum += source[x];
            ++count;
        }
    }
    if (count == 0 || sum > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return -1;
    }
    return static_cast<std::int64_t>(sum / count);
}

} // namespace

bool EmbedE2eMediaMarker(
        VideoFrame& frame,
        const E2eMediaMarker& marker) {
    if (frame.type() != VideoBufferType::I420 || !frame.data() ||
        marker.probe_id == 0 || marker.sequence == 0) {
        return false;
    }
    const auto region = RegionFor(frame.width(), frame.height());
    const auto expected_y_size = static_cast<std::size_t>(frame.width()) *
        static_cast<std::size_t>(frame.height());
    if (!region || frame.dataSize() < expected_y_size) return false;

    const auto bytes = EncodeMarker(marker);
    for (std::size_t bit_index = 0;
         bit_index < kMarkerBytes * 8;
         ++bit_index) {
        const bool bit = (bytes[bit_index / 8] &
            (0x80U >> (bit_index % 8))) != 0;
        const int row = static_cast<int>(bit_index / kBitsPerRow);
        const int first_column = static_cast<int>(bit_index % kBitsPerRow) * 2;
        FillCell(frame.data(), frame.width(), *region, first_column, row,
            bit ? kHighLuma : kLowLuma);
        FillCell(frame.data(), frame.width(), *region, first_column + 1, row,
            bit ? kLowLuma : kHighLuma);
    }
    return true;
}

std::optional<E2eMediaMarker> DecodeE2eMediaMarker(
        const VideoFrame& frame) {
    if (frame.type() != VideoBufferType::I420 || !frame.data()) {
        return std::nullopt;
    }
    return DecodeE2eMediaMarkerFromLuma(
        frame.data(), frame.width(), frame.width(), frame.height());
}

std::optional<E2eMediaMarker> DecodeE2eMediaMarker(
        const render::OwnedI420Frame& frame) {
    return DecodeE2eMediaMarkerFromLuma(
        frame.data_y(), frame.stride_y(), frame.width(), frame.height());
}

std::optional<E2eMediaMarker> DecodeE2eMediaMarkerFromLuma(
        const std::uint8_t* data_y,
        int stride_y,
        int width,
        int height) {
    if (!data_y || stride_y < width) return std::nullopt;
    const auto region = RegionFor(width, height);
    if (!region) return std::nullopt;

    std::array<std::uint8_t, kMarkerBytes> bytes{};
    for (std::size_t bit_index = 0;
         bit_index < kMarkerBytes * 8;
         ++bit_index) {
        const int row = static_cast<int>(bit_index / kBitsPerRow);
        const int first_column = static_cast<int>(bit_index % kBitsPerRow) * 2;
        const auto first = CellAverage(
            data_y, stride_y, *region, first_column, row);
        const auto second = CellAverage(
            data_y, stride_y, *region, first_column + 1, row);
        if (first < 0 || second < 0 ||
            (first > second ? first - second : second - first) <
                kMinimumPairDelta) {
            return std::nullopt;
        }
        if (first > second) {
            bytes[bit_index / 8] |= static_cast<std::uint8_t>(
                0x80U >> (bit_index % 8));
        }
    }

    if (bytes[0] != kMagic0 || bytes[1] != kMagic1 ||
        bytes[2] != kE2eMediaMarkerVersion || bytes[3] != 0 ||
        Load32(bytes.data() + 16) != Crc32(bytes.data(), kPayloadBytes)) {
        return std::nullopt;
    }
    E2eMediaMarker marker;
    marker.probe_id = Load64(bytes.data() + 4);
    marker.sequence = Load32(bytes.data() + 12);
    if (marker.probe_id == 0 || marker.sequence == 0) return std::nullopt;
    return marker;
}

} // namespace livekit::telemetry
