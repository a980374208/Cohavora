#pragma once

#include <cstdint>
#include <optional>

namespace livekit {
class VideoFrame;

namespace render {
class OwnedI420Frame;
}

namespace telemetry {

inline constexpr std::uint8_t kE2eMediaMarkerVersion = 1;

struct E2eMediaMarker {
    std::uint64_t probe_id = 0;
    std::uint32_t sequence = 0;
};

// The marker is intentionally limited to controlled synthetic I420 media. It
// occupies a fixed relative luma region so codec scaling preserves the grid.
[[nodiscard]] bool EmbedE2eMediaMarker(
    VideoFrame& frame,
    const E2eMediaMarker& marker);

[[nodiscard]] std::optional<E2eMediaMarker> DecodeE2eMediaMarker(
    const VideoFrame& frame);
[[nodiscard]] std::optional<E2eMediaMarker> DecodeE2eMediaMarker(
    const render::OwnedI420Frame& frame);
[[nodiscard]] std::optional<E2eMediaMarker> DecodeE2eMediaMarkerFromLuma(
    const std::uint8_t* data_y,
    int stride_y,
    int width,
    int height);

} // namespace telemetry
} // namespace livekit
