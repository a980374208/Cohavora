#include "src/ui/whiteboard/whiteboard_panel.h"
#include "src/ui/app_translation.h"
#include "src/core/whiteboard/whiteboard_asset.h"
#include <QtWidgets/QApplication>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QTimer>
#include <QtCore/QDebug>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QSettings>
#include <QtGui/QImageReader>
#include <QtPlugin>

Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin)
Q_IMPORT_PLUGIN(QSvgPlugin)
Q_IMPORT_PLUGIN(QSvgIconPlugin)

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (QApplication::platformName() != QStringLiteral("windows")) return 77;
    const auto args = app.arguments();
    const int stateArgument = args.indexOf(QStringLiteral("--state-file"));
    if (stateArgument < 0 || stateArgument + 1 >= args.size()) return 2;
    const QString stateFile = args[stateArgument + 1];
    QCoreApplication::setOrganizationName(QStringLiteral("LiveKitUiaFixture"));
    QCoreApplication::setApplicationName(QStringLiteral("WhiteboardFiles"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QFileInfo(stateFile).absolutePath());
    MeetingUI::AppTranslation::install(app, MeetingUI::AppTranslation::startupLocale(args));
    MeetingUI::WhiteboardPanel panel;
    QImage input(320, 180, QImage::Format_RGB32);
    for (int y = 0; y < input.height(); ++y)
        for (int x = 0; x < input.width(); ++x)
            input.setPixel(x, y, qRgb(x % 256, y % 256, (x + y) % 256));
    if (!input.save(QFileInfo(stateFile).dir().filePath(QStringLiteral("input image.png")), "PNG")) return 2;
    QImage existing(320, 180, QImage::Format_RGB32);
    existing.fill(qRgb(255, 0, 255));
    if (!existing.save(QFileInfo(stateFile).dir().filePath(QStringLiteral("existing image.png")), "PNG")) return 2;
    // Invalid import data is created before show; never used as a UI backdoor.
    QSaveFile invalid(QFileInfo(stateFile).dir().filePath(QStringLiteral("invalid content.png")));
    const QByteArray invalidBytes("This is not PNG or JPEG content.\n");
    if (!invalid.open(QIODevice::WriteOnly) || invalid.write(invalidBytes) != invalidBytes.size() || !invalid.commit()) return 2;
    QImage small(16, 16, QImage::Format_RGB32);
    small.fill(Qt::green);
    if (!small.save(QFileInfo(stateFile).dir().filePath(QStringLiteral("invalid dimensions.png")), "PNG")) return 2;
    QFile complete(QFileInfo(stateFile).dir().filePath(QStringLiteral("input image.png")));
    if (!complete.open(QIODevice::ReadOnly)) return 2;
    const auto truncatedBytes = complete.read(80); // IHDR, pHYs and a partial IDAT.
    if (truncatedBytes.size() != 80) return 2;
    const auto truncatedPath = QFileInfo(stateFile).dir().filePath(QStringLiteral("truncated image.png"));
    QSaveFile truncated(truncatedPath);
    if (!truncated.open(QIODevice::WriteOnly) ||
        truncated.write(truncatedBytes) != truncatedBytes.size() || !truncated.commit()) return 2;
    QImageReader truncatedReader(truncatedPath);
    truncatedReader.setDecideFormatFromContent(true);
    const auto format = truncatedReader.format().toLower();
    const auto size = truncatedReader.size();
    const auto decoded = truncatedReader.read();
    const auto error = MeetingUI::loadAndNormalizeWhiteboardImage(truncatedPath).error;
    if (format != "png" || size != QSize(320, 180) || !decoded.isNull() ||
        error != MeetingUI::WhiteboardImageError::DecodeFailed) {
        qWarning() << "Truncated PNG fixture:" << format << size << decoded.isNull()
                   << static_cast<int>(error) << truncatedReader.errorString();
        return 2;
    }
    QImage oversized(2048, 2048, QImage::Format_RGB32);
    if (oversized.isNull()) return 2;
    quint32 noise = 0x6d2b79f5u;
    for (int y = 0; y < oversized.height(); ++y) {
        auto *pixels = reinterpret_cast<QRgb *>(oversized.scanLine(y));
        for (int x = 0; x < oversized.width(); ++x) {
            noise ^= noise << 13;
            noise ^= noise >> 17;
            noise ^= noise << 5;
            pixels[x] = qRgb(noise & 0xff, (noise >> 8) & 0xff, (noise >> 16) & 0xff);
        }
    }
    const auto oversizedPath = QFileInfo(stateFile).dir().filePath(QStringLiteral("oversized image.png"));
    if (!oversized.save(oversizedPath, "PNG") ||
        QFileInfo(oversizedPath).size() <= static_cast<qint64>(livekit::whiteboard::MaxAssetBytes)) return 2;
    QImageReader oversizedReader(oversizedPath);
    oversizedReader.setDecideFormatFromContent(true);
    if (oversizedReader.format().toLower() != "png" ||
        oversizedReader.size() != QSize(2048, 2048) ||
        MeetingUI::loadAndNormalizeWhiteboardImage(oversizedPath).error != MeetingUI::WhiteboardImageError::InputTooLarge)
        return 2;
    QJsonObject expectedErrors;
    expectedErrors["unsupported"] = MeetingUI::WhiteboardPanel::tr("Only PNG and JPEG image content is supported.");
    expectedErrors["dimensions"] = MeetingUI::WhiteboardPanel::tr("The image dimensions are unsupported (320x180 to 4096x4096, at most 16 MP).");
    expectedErrors["truncated"] = MeetingUI::WhiteboardPanel::tr("The image could not be decoded.");
    expectedErrors["oversized"] = MeetingUI::WhiteboardPanel::tr("The image file exceeds the 8 MiB limit.");
    QSaveFile manifest(QFileInfo(stateFile).dir().filePath(QStringLiteral("expected-errors.json")));
    const auto manifestBytes = QJsonDocument(expectedErrors).toJson();
    if (!manifest.open(QIODevice::WriteOnly) || manifest.write(manifestBytes) != manifestBytes.size() || !manifest.commit()) return 2;
    panel.resize(1200, 760);
    panel.show();
    int sample = 0;
    auto observe = [&] {
        const auto &state = panel.document();
        QJsonObject value;
        value["pid"] = static_cast<double>(QCoreApplication::applicationPid());
        value["sample"] = ++sample;
        value["pages"] = static_cast<int>(state.pages().size());
        value["width"] = state.page().width;
        value["height"] = state.page().height;
        value["background"] = QString::fromStdString(state.page().backgroundAssetId);
        value["backgroundReady"] = !panel.canvas()->currentBackgroundImage().isNull();
        value["objects"] = static_cast<int>(state.page().objects.size());
        value["epoch"] = static_cast<double>(state.page().epoch);
        value["pageId"] = QString::fromStdString(state.page().id);
        value["canUndo"] = state.canUndo(state.owner());
        value["canRedo"] = state.canRedo(state.owner());
        value["snapshot"] = QString::fromStdString(state.toJson());
        // Immutable numbered samples: readers never compete with replacement
        // of an existing Windows file. Only completed atomic writes are read.
        QSaveFile file(stateFile + QStringLiteral(".%1").arg(sample, 8, 10, QLatin1Char('0')));
        if (!file.open(QIODevice::WriteOnly)) { qWarning() << file.errorString(); app.exit(2); return; }
        const auto bytes = QJsonDocument(value).toJson();
        if (file.write(bytes) != bytes.size() || !file.commit()) { qWarning() << file.errorString(); app.exit(2); }
    };
    QTimer observer;
    QObject::connect(&observer, &QTimer::timeout, &app, observe);
    observer.start(200);
    QTimer::singleShot(0, &app, observe);
    QTimer::singleShot(90000, &app, [&app] { app.exit(3); });
    return app.exec();
}
