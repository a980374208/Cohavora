#include "whiteboard_image_loader.h"

#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtGui/QImageReader>
#include <QtGui/QPainter>

namespace MeetingUI {
namespace wb = livekit::whiteboard;
namespace {

bool validDimensions(const QSize &size) {
    return size.width() > 0 && size.height() > 0 &&
        size.width() <= static_cast<int>(wb::MaxAssetDimension) &&
        size.height() <= static_cast<int>(wb::MaxAssetDimension) &&
        static_cast<std::uint64_t>(size.width()) * size.height() <= wb::MaxAssetPixels;
}

WhiteboardImageResult decodeSource(const QByteArray &source) {
    WhiteboardImageResult result;
    QBuffer input;
    input.setData(source);
    if (!input.open(QIODevice::ReadOnly)) {
        result.error = WhiteboardImageError::ReadFailed;
        return result;
    }
    QImageReader reader(&input);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    const auto format = reader.format().toLower();
    if (format != "png" && format != "jpg" && format != "jpeg") {
        result.error = WhiteboardImageError::UnsupportedFormat;
        return result;
    }
    if (!validDimensions(reader.size())) {
        result.error = WhiteboardImageError::InvalidDimensions;
        return result;
    }
    const auto decoded = reader.read();
    if (decoded.isNull()) {
        result.error = WhiteboardImageError::DecodeFailed;
        return result;
    }
    if (!validDimensions(decoded.size()) || decoded.width() < 320 || decoded.height() < 180) {
        result.error = WhiteboardImageError::InvalidDimensions;
        return result;
    }

    QImage normalized(decoded.size(), QImage::Format_RGB32);
    if (normalized.isNull()) {
        result.error = WhiteboardImageError::DecodeFailed;
        return result;
    }
    normalized.fill(Qt::white);
    {
        QPainter painter(&normalized);
        painter.drawImage(QPoint(0, 0), decoded);
    }
    QByteArray png;
    QBuffer output(&png);
    if (!output.open(QIODevice::WriteOnly) || !normalized.save(&output, "PNG") ||
        png.isEmpty() || png.size() > static_cast<qsizetype>(wb::MaxAssetBytes)) {
        result.error = WhiteboardImageError::OutputTooLarge;
        return result;
    }
    result.asset.mime = "image/png";
    result.asset.width = static_cast<std::uint32_t>(normalized.width());
    result.asset.height = static_cast<std::uint32_t>(normalized.height());
    result.asset.bytes.assign(png.constData(), static_cast<std::size_t>(png.size()));
    result.asset.id = wb::assetContentId(result.asset.bytes);
    result.image = std::move(normalized);
    return result;
}

} // namespace

WhiteboardImageResult loadAndNormalizeWhiteboardImage(const QString &path) {
    WhiteboardImageResult result;
    const QFileInfo info(path);
    if (!info.isFile()) {
        result.error = WhiteboardImageError::ReadFailed;
        return result;
    }
    if (info.size() <= 0 || info.size() > static_cast<qint64>(wb::MaxAssetBytes)) {
        result.error = WhiteboardImageError::InputTooLarge;
        return result;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = WhiteboardImageError::ReadFailed;
        return result;
    }
    const auto bytes = file.read(wb::MaxAssetBytes + 1);
    if (bytes.size() != info.size()) {
        result.error = bytes.size() > static_cast<qsizetype>(wb::MaxAssetBytes)
            ? WhiteboardImageError::InputTooLarge : WhiteboardImageError::ReadFailed;
        return result;
    }
    return decodeSource(bytes);
}

WhiteboardImageResult decodeWhiteboardAsset(const wb::Asset &asset) {
    if (!wb::validAsset(asset)) {
        WhiteboardImageResult result;
        result.error = WhiteboardImageError::IntegrityFailed;
        return result;
    }
    auto result = decodeWhiteboardAssetBytes(QString::fromStdString(asset.id),
        QByteArray(asset.bytes.data(), static_cast<int>(asset.bytes.size())));
    if (!result.ok() || result.asset.width != asset.width || result.asset.height != asset.height) {
        result = {};
        result.error = WhiteboardImageError::IntegrityFailed;
    }
    return result;
}

WhiteboardImageResult decodeWhiteboardAssetBytes(const QString &assetId, const QByteArray &png) {
    WhiteboardImageResult result;
    if (!wb::validAssetId(assetId.toStdString()) || png.isEmpty() ||
        png.size() > static_cast<qsizetype>(wb::MaxAssetBytes) ||
        wb::assetContentId(std::string_view(png.constData(), static_cast<std::size_t>(png.size()))) !=
            assetId.toStdString()) {
        result.error = WhiteboardImageError::IntegrityFailed;
        return result;
    }
    QBuffer input;
    input.setData(png);
    if (!input.open(QIODevice::ReadOnly)) {
        result.error = WhiteboardImageError::DecodeFailed;
        return result;
    }
    QImageReader reader(&input);
    reader.setDecideFormatFromContent(true);
    if (reader.format().toLower() != "png" || !validDimensions(reader.size())) {
        result.error = WhiteboardImageError::IntegrityFailed;
        return result;
    }
    const auto decoded = reader.read();
    if (decoded.isNull() || !validDimensions(decoded.size()) ||
        decoded.width() < 320 || decoded.height() < 180) {
        result = {};
        result.error = WhiteboardImageError::IntegrityFailed;
        return result;
    }
    result.image = decoded.convertToFormat(QImage::Format_RGB32);
    result.asset.id = assetId.toStdString();
    result.asset.mime = "image/png";
    result.asset.width = static_cast<std::uint32_t>(decoded.width());
    result.asset.height = static_cast<std::uint32_t>(decoded.height());
    result.asset.bytes.assign(png.constData(), static_cast<std::size_t>(png.size()));
    return result;
}

} // namespace MeetingUI
