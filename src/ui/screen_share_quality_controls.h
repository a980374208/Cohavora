#pragma once

#include "src/media/screen_share_quality.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QSignalBlocker>
#include <QtCore/QSize>
#include <QtGui/QScreen>
#include <QtWidgets/QComboBox>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace MeetingUI {

inline QSize screenShareDisplayPixelSize(const QScreen *screen) {
    if (!screen) return {};
#if defined(Q_OS_WIN)
    // Qt geometry is in logical pixels. Display mode dimensions remain the
    // physical pixel size even when Windows or Qt applies display scaling.
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    const auto device = screen->name().toStdWString();
    if (!device.empty() && EnumDisplaySettingsExW(device.c_str(), ENUM_CURRENT_SETTINGS, &mode, 0))
        return QSize(int(mode.dmPelsWidth), int(mode.dmPelsHeight));
    return {};
#else
    const auto size = screen->size();
    const auto ratio = screen->devicePixelRatio();
    return QSize(qRound(size.width() * ratio), qRound(size.height() * ratio));
#endif
}

inline bool screenShareResolutionFits(QSize pixels, QSize tier) {
    if (pixels.width() <= 0 || pixels.height() <= 0) return false;
    // Compare both axes in the source orientation, including portrait screens.
    if (pixels.height() > pixels.width()) tier.transpose();
    return tier.width() <= pixels.width() && tier.height() <= pixels.height();
}

inline void populateScreenShareResolutionChoices(QComboBox &combo, QSize maximumPixels) {
    using Resolution = livekit::ScreenShareResolution;
    const QSignalBlocker blocker(&combo);
    combo.clear();
    // Display order is independent of the persisted enum values (Native == 4).
    combo.addItem(QCoreApplication::translate("MeetingUI", "Auto (up to 2K)"), static_cast<int>(Resolution::Auto));
    if (screenShareResolutionFits(maximumPixels, QSize(1280, 720)))
        combo.addItem(QCoreApplication::translate("MeetingUI", "720p"), static_cast<int>(Resolution::P720));
    if (screenShareResolutionFits(maximumPixels, QSize(1920, 1080)))
        combo.addItem(QCoreApplication::translate("MeetingUI", "1080p"), static_cast<int>(Resolution::P1080));
    if (screenShareResolutionFits(maximumPixels, QSize(2560, 1440)))
        combo.addItem(QCoreApplication::translate("MeetingUI", "1440p (2K)"), static_cast<int>(Resolution::P1440));
    if (screenShareResolutionFits(maximumPixels, QSize(3840, 2160)))
        combo.addItem(QCoreApplication::translate("MeetingUI", "2160p (4K)"), static_cast<int>(Resolution::P2160));
    combo.addItem(QCoreApplication::translate("MeetingUI", "Native (up to 4K)"), static_cast<int>(Resolution::Native));
}

inline void selectScreenShareResolution(QComboBox &combo, int resolution) {
    const int index = combo.findData(resolution);
    combo.setCurrentIndex(index >= 0 ? index : combo.findData(static_cast<int>(livekit::ScreenShareResolution::Auto)));
}

} // namespace MeetingUI
