#include "whiteboard_asset.h"

#include <openssl/sha.h>

#include <algorithm>
#include <array>

namespace livekit::whiteboard {

std::string assetContentId(std::string_view bytes) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), digest.data());
    constexpr char digits[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        result[i * 2] = digits[digest[i] >> 4];
        result[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    return result;
}

bool validAssetId(std::string_view id) {
    return id.size() == SHA256_DIGEST_LENGTH * 2 &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
}

bool validAsset(const Asset &asset) {
    if (!validAssetId(asset.id) || asset.mime != "image/png" || asset.bytes.empty() ||
        asset.bytes.size() > MaxAssetBytes || asset.width == 0 || asset.height == 0 ||
        asset.width > MaxAssetDimension || asset.height > MaxAssetDimension) return false;
    const auto pixels = static_cast<std::uint64_t>(asset.width) * asset.height;
    return pixels <= MaxAssetPixels && assetContentId(asset.bytes) == asset.id;
}

} // namespace livekit::whiteboard
