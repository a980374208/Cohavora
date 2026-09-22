#pragma once

#include "src/core/whiteboard/whiteboard_asset.h"

#include <QtCore/QString>
#include <QtGui/QImage>

namespace MeetingUI {

enum class WhiteboardImageError {
    None,
    ReadFailed,
    InputTooLarge,
    UnsupportedFormat,
    InvalidDimensions,
    DecodeFailed,
    OutputTooLarge,
    IntegrityFailed,
};

struct WhiteboardImageResult {
    livekit::whiteboard::Asset asset;
    QImage image;
    WhiteboardImageError error = WhiteboardImageError::None;
    bool ok() const { return error == WhiteboardImageError::None && !image.isNull(); }
};

WhiteboardImageResult loadAndNormalizeWhiteboardImage(const QString &path);
WhiteboardImageResult decodeWhiteboardAsset(const livekit::whiteboard::Asset &asset);
WhiteboardImageResult decodeWhiteboardAssetBytes(const QString &assetId, const QByteArray &png);

} // namespace MeetingUI
