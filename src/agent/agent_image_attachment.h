#pragma once

#ifdef QT_GUI_LIB

#include <QtCore/QBuffer>
#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QIODevice>
#include <QtCore/QString>
#include <QtCore/QUuid>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtGui/QImageReader>
#include <QtGui/QImageWriter>

namespace OpenMeeting {

namespace detail {

[[nodiscard]] inline QImage flattenAlphaOntoWhite(const QImage &src) {
	if (!src.hasAlphaChannel()) {
		return src.convertToFormat(QImage::Format_RGB888);
	}
	QImage rgba = src.convertToFormat(QImage::Format_ARGB32);
	const int w = rgba.width();
	const int h = rgba.height();
	for (int y = 0; y < h; ++y) {
		auto *line = reinterpret_cast<QRgb *>(rgba.scanLine(y));
		for (int x = 0; x < w; ++x) {
			const QRgb px = line[x];
			const int a = qAlpha(px);
			if (a < 255) {
				const int inv = 255 - a;
				line[x] = qRgb(
					(qRed(px) * a + 255 * inv) / 255,
					(qGreen(px) * a + 255 * inv) / 255,
					(qBlue(px) * a + 255 * inv) / 255);
			}
		}
	}
	return rgba.convertToFormat(QImage::Format_RGB888);
}

[[nodiscard]] inline QByteArray encodeAgentQImage(const QImage &img, const char *format, int quality) {
	QByteArray out;
	QBuffer buf(&out);
	if (!buf.open(QIODevice::WriteOnly)) {
		return {};
	}
	QImageWriter writer(&buf, format);
	if (quality >= 0) {
		writer.setQuality(quality);
	}
	if (!writer.write(img)) {
		return {};
	}
	return out;
}

} // namespace detail

[[nodiscard]] inline AgentImageAttachment prepareAgentImageFromQImage(
		const QImage &srcImg,
		const QString &fileName,
		qint64 originalSize = 0,
		QString *outError = nullptr) {
	if (srcImg.isNull() || srcImg.width() < 1 || srcImg.height() < 1) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI", "Unsupported or corrupted image file (only PNG, JPEG, WebP, and GIF are allowed).");
		}
		return {};
	}
	if (srcImg.width() > kMaxAgentImageSourceDimension ||
		srcImg.height() > kMaxAgentImageSourceDimension ||
		qint64(srcImg.width()) * srcImg.height() > kMaxAgentImageSourcePixels) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI", "Image resolution exceeds the supported limit.");
		}
		return {};
	}

	QImage working = srcImg;
	if (working.width() > kMaxAgentImageDimension || working.height() > kMaxAgentImageDimension) {
		working = working.scaled(
			kMaxAgentImageDimension,
			kMaxAgentImageDimension,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}

	QString mime;
	QByteArray encoded;
	constexpr qint64 kMaxPngPreferredBytes = 1536LL * 1024;
	if (working.hasAlphaChannel()) {
		const QByteArray pngBytes = detail::encodeAgentQImage(working, "png", -1);
		if (!pngBytes.isEmpty() && pngBytes.size() <= kMaxPngPreferredBytes) {
			mime = QStringLiteral("image/png");
			encoded = pngBytes;
		}
	}
	if (encoded.isEmpty()) {
		const QImage flat = detail::flattenAlphaOntoWhite(working);
		const QByteArray jpgBytes = detail::encodeAgentQImage(flat, "jpg", 85);
		if (!jpgBytes.isEmpty()) {
			mime = QStringLiteral("image/jpeg");
			encoded = jpgBytes;
		} else {
			// Fallback to PNG (built into QtGui) if the JPEG plugin is unavailable.
			const QByteArray pngBytes = detail::encodeAgentQImage(working, "png", -1);
			if (!pngBytes.isEmpty()) {
				mime = QStringLiteral("image/png");
				encoded = pngBytes;
			}
		}
	}
	if (encoded.isEmpty() || encoded.size() > kMaxEncodedAgentImageBytes) {
		if (working.width() > 1280 || working.height() > 1280) {
			working = working.scaled(1280, 1280, Qt::KeepAspectRatio, Qt::SmoothTransformation);
		}
		const QImage flat = detail::flattenAlphaOntoWhite(working);
		QByteArray fallbackBytes = detail::encodeAgentQImage(flat, "jpg", 75);
		QString fallbackMime = QStringLiteral("image/jpeg");
		if (fallbackBytes.isEmpty()) {
			fallbackBytes = detail::encodeAgentQImage(working, "png", -1);
			fallbackMime = QStringLiteral("image/png");
		}
		if (!fallbackBytes.isEmpty() && fallbackBytes.size() <= kMaxEncodedAgentImageBytes) {
			mime = fallbackMime;
			encoded = fallbackBytes;
		} else {
			if (outError) {
				*outError = QCoreApplication::translate("MeetingUI", "Image file exceeds the 10 MB limit.");
			}
			return {};
		}
	}

	AgentImageAttachment att;
	att.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	att.fileName = fileName.trimmed().isEmpty() ? QStringLiteral("image.png") : fileName.trimmed();
	att.mimeType = mime;
	att.encodedBytes = encoded;
	att.width = working.width();
	att.height = working.height();
	att.originalSize = originalSize > 0 ? originalSize : encoded.size();
	return att;
}

[[nodiscard]] inline AgentImageAttachment prepareAgentImageFromRawBytes(
		const QByteArray &rawBytes,
		const QString &fileName,
		QString *outError = nullptr) {
	if (rawBytes.isEmpty()) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI", "Unsupported or corrupted image file (only PNG, JPEG, WebP, and GIF are allowed).");
		}
		return {};
	}
	if (rawBytes.size() > kMaxRawAgentImageBytes) {
		if (outError) {
			*outError = QCoreApplication::translate("MeetingUI", "Image file exceeds the 10 MB limit.");
		}
		return {};
	}
	const QString detectedMime = detectSupportedAgentImageMimeByMagic(rawBytes);
	if (detectedMime.isEmpty()) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI", "Unsupported or corrupted image file (only PNG, JPEG, WebP, and GIF are allowed).");
		}
		return {};
	}

	const char *readerFmt = nullptr;
	if (detectedMime == QStringLiteral("image/png")) {
		readerFmt = "png";
	} else if (detectedMime == QStringLiteral("image/jpeg")) {
		readerFmt = "jpg";
	} else if (detectedMime == QStringLiteral("image/webp")) {
		readerFmt = "webp";
	} else if (detectedMime == QStringLiteral("image/gif")) {
		readerFmt = "gif";
	}

	QBuffer buf;
	buf.setData(rawBytes);
	if (!buf.open(QIODevice::ReadOnly)) {
		return {};
	}
	QImageReader reader(&buf, readerFmt ? QByteArray(readerFmt) : QByteArray());
	reader.setAutoTransform(true);
	const QSize declaredSize = reader.size();
	if (declaredSize.isValid() &&
		(declaredSize.width() < 1 || declaredSize.height() < 1 ||
		 declaredSize.width() > kMaxAgentImageSourceDimension ||
		 declaredSize.height() > kMaxAgentImageSourceDimension ||
		 qint64(declaredSize.width()) * declaredSize.height() > kMaxAgentImageSourcePixels)) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI", "Image resolution exceeds the supported limit.");
		}
		return {};
	}
	const QImage decoded = reader.read();
	if (decoded.isNull()) {
		if (outError) {
			*outError = QCoreApplication::translate(
				"MeetingUI", "Unsupported or corrupted image file (only PNG, JPEG, WebP, and GIF are allowed).");
		}
		return {};
	}
	return prepareAgentImageFromQImage(decoded, fileName, rawBytes.size(), outError);
}

} // namespace OpenMeeting

#endif // QT_GUI_LIB
