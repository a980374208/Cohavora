#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace livekit::whiteboard {

struct Asset {
    std::string id;
    std::string mime = "image/png";
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string bytes;
};

inline constexpr std::size_t MaxAssetBytes = 8 * 1024 * 1024;
inline constexpr std::size_t MaxAssetTotalBytes = 16 * 1024 * 1024;
inline constexpr std::size_t AssetChunkBytes = 6000;
inline constexpr std::size_t MaxAssets = 8;
inline constexpr std::uint64_t MaxAssetPixels = 16ULL * 1024 * 1024;
inline constexpr std::uint32_t MaxAssetDimension = 4096;

std::string assetContentId(std::string_view bytes);
bool validAssetId(std::string_view id);
bool validAsset(const Asset &asset);

} // namespace livekit::whiteboard
