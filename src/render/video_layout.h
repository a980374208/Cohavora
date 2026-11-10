#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace livekit::render {
// UI layout and input use logical pixels; ScaleVideoTile produces physical
// pixels for backend draw calls. Both origins are top-left.
struct VideoClipRect {
    int x = 0, y = 0, width = 0, height = 0;
    bool operator==(const VideoClipRect&) const = default;
};
struct VideoTileRect {
    std::string identity;
    int x = 0, y = 0, width = 0, height = 0;
    bool isSpeaking = false;
    float audioLevel = 0.0f;
    bool hasVideo = false;
    // Independent visible region in canvas coordinates. Keep the full card
    // destination for aspect fitting and use this only for scissor/input.
    std::optional<VideoClipRect> clip;
};
struct VideoFrameGeometry {
    int width = 0, height = 0;
    uint32_t rotation = 0;
    bool available = false;
};
inline VideoTileRect ScaleVideoTile(VideoTileRect tile, int logicalWidth, int logicalHeight,
                                    int pixelWidth, int pixelHeight) {
    if (logicalWidth <= 0 || logicalHeight <= 0) { tile.width = tile.height = 0; return tile; }
    const auto x = [&](int v) { return int(std::lround(v * double(pixelWidth) / logicalWidth)); };
    const auto y = [&](int v) { return int(std::lround(v * double(pixelHeight) / logicalHeight)); };
    const int right = x(tile.x + tile.width), bottom = y(tile.y + tile.height);
    tile.x = x(tile.x); tile.y = y(tile.y);
    tile.width = right - tile.x; tile.height = bottom - tile.y;
    if (tile.clip) {
        auto& clip = *tile.clip;
        const int clipRight = x(clip.x + clip.width), clipBottom = y(clip.y + clip.height);
        clip.x = x(clip.x); clip.y = y(clip.y);
        clip.width = clipRight - clip.x; clip.height = clipBottom - clip.y;
    }
    return tile;
}
inline VideoTileRect FitVideoTile(VideoTileRect tile, VideoFrameGeometry frame) {
    if (frame.rotation == 90 || frame.rotation == 270) std::swap(frame.width, frame.height);
    if (frame.width <= 0 || frame.height <= 0) return tile;
    const double scale = std::min(double(tile.width) / frame.width, double(tile.height) / frame.height);
    const int w = std::max(1, int(std::lround(frame.width * scale)));
    const int h = std::max(1, int(std::lround(frame.height * scale)));
    tile.x += (tile.width - w) / 2; tile.y += (tile.height - h) / 2;
    tile.width = w; tile.height = h;
    return tile;
}
} // namespace livekit::render
