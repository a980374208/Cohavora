#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace livekit::render {
// UI layout and input use logical pixels; ScaleVideoTile produces physical
// pixels for backend draw calls. Both origins are top-left.
struct VideoTileRect {
    std::string identity;
    int x = 0, y = 0, width = 0, height = 0;
    bool isSpeaking = false;
    float audioLevel = 0.0f;
    bool hasVideo = false;
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
