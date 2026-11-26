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

inline QString screenShareResolutionBaseLabel(livekit::ScreenShareResolution resolution) {
    using Resolution = livekit::ScreenShareResolution;
    switch (resolution) {
    case Resolution::Auto: return QCoreApplication::translate("MeetingUI", "Auto (up to 2K)");
    case Resolution::P720: return QCoreApplication::translate("MeetingUI", "720p");
    case Resolution::P1080: return QCoreApplication::translate("MeetingUI", "1080p");
    case Resolution::P1440: return QCoreApplication::translate("MeetingUI", "1440p (2K)");
    case Resolution::P2160: return QCoreApplication::translate("MeetingUI", "2160p (4K)");
    case Resolution::Native: return QCoreApplication::translate("MeetingUI", "Native (up to 4K)");
    }
    return QCoreApplication::translate("MeetingUI", "Auto (up to 2K)");
}

inline QString screenShareResolutionShortLabel(livekit::ScreenShareResolution resolution) {
    using Resolution = livekit::ScreenShareResolution;
    switch (resolution) {
    case Resolution::P720: return QCoreApplication::translate("MeetingUI", "720p");
    case Resolution::P1080: return QCoreApplication::translate("MeetingUI", "1080p");
    case Resolution::P1440: return QCoreApplication::translate("MeetingUI", "1440p (2K)");
    case Resolution::P2160: return QCoreApplication::translate("MeetingUI", "2160p (4K)");
    case Resolution::Auto: return QCoreApplication::translate("MeetingUI", "Auto (up to 2K)");
    case Resolution::Native: return QCoreApplication::translate("MeetingUI", "Native (up to 4K)");
    }
    return QCoreApplication::translate("MeetingUI", "720p");
}

inline QString screenShareConstraintLevelLabel(const livekit::ScreenShareQualityConstraint &constraint) {
    if (constraint.level_idc <= 0) return {};
    if (constraint.reason == livekit::ScreenShareConstraintReason::H265Level) {
        return QStringLiteral("%1.%2").arg(constraint.level_idc / 30).arg((constraint.level_idc % 30) / 3);
    }
    return QStringLiteral("%1.%2").arg(constraint.level_idc / 10).arg(constraint.level_idc % 10);
}

inline QString screenShareConstraintSourceLabel(const livekit::ScreenShareQualityConstraint &constraint) {
    using Reason = livekit::ScreenShareConstraintReason;
    QString base;
    switch (constraint.reason) {
    case Reason::H264SoftwareLevel:
        base = QCoreApplication::translate("MeetingUI", "H.264 software encoder");
        break;
    case Reason::H264HardwareLevel:
        base = QCoreApplication::translate("MeetingUI", "H.264 hardware encoder");
        break;
    case Reason::H264NegotiatedLevel:
        base = QCoreApplication::translate("MeetingUI", "H.264 negotiated session");
        break;
    case Reason::H265Level:
        base = QCoreApplication::translate("MeetingUI", "H.265 encoder");
        break;
    case Reason::None:
        return {};
    }
    const auto level = screenShareConstraintLevelLabel(constraint);
    return level.isEmpty() ? base
        : QCoreApplication::translate("MeetingUI", "%1 (Level %2)").arg(base, level);
}

inline bool screenShareResolutionExceedsConstraint(
        livekit::ScreenShareResolution resolution,
        QSize maximumPixels,
        const livekit::ScreenShareQualityConstraint &constraint) {
    if (!constraint.has_cap()) return false;
    const int srcW = maximumPixels.width() >= 2 ? maximumPixels.width() : 3840;
    const int srcH = maximumPixels.height() >= 2 ? maximumPixels.height() : 2160;
    const auto target = livekit::ResolveScreenShareProfile(srcW, srcH, {resolution, 20});
    if (!target) {
        // Native on >4K sources exceeds any sub-4K codec ceiling.
        return resolution == livekit::ScreenShareResolution::Native;
    }
    int capW = constraint.max_width;
    int capH = constraint.max_height;
    if (srcH > srcW) std::swap(capW, capH);
    return target->width > capW || target->height > capH;
}

inline void populateScreenShareResolutionChoices(QComboBox &combo, QSize maximumPixels) {
    using Resolution = livekit::ScreenShareResolution;
    const QSignalBlocker blocker(&combo);
    combo.clear();
    // Display order is independent of the persisted enum values (Native == 4).
    combo.addItem(screenShareResolutionBaseLabel(Resolution::Auto), static_cast<int>(Resolution::Auto));
    if (screenShareResolutionFits(maximumPixels, QSize(1280, 720)))
        combo.addItem(screenShareResolutionBaseLabel(Resolution::P720), static_cast<int>(Resolution::P720));
    if (screenShareResolutionFits(maximumPixels, QSize(1920, 1080)))
        combo.addItem(screenShareResolutionBaseLabel(Resolution::P1080), static_cast<int>(Resolution::P1080));
    if (screenShareResolutionFits(maximumPixels, QSize(2560, 1440)))
        combo.addItem(screenShareResolutionBaseLabel(Resolution::P1440), static_cast<int>(Resolution::P1440));
    if (screenShareResolutionFits(maximumPixels, QSize(3840, 2160)))
        combo.addItem(screenShareResolutionBaseLabel(Resolution::P2160), static_cast<int>(Resolution::P2160));
    combo.addItem(screenShareResolutionBaseLabel(Resolution::Native), static_cast<int>(Resolution::Native));
}

inline void annotateScreenShareResolutionChoices(
        QComboBox &combo,
        QSize maximumPixels,
        const livekit::ScreenShareQualityConstraint &constraint) {
    const QSignalBlocker blocker(&combo);
    const QString capLabel = constraint.has_cap()
        ? screenShareResolutionShortLabel(*constraint.max_resolution) : QString();
    const QString sourceLabel = screenShareConstraintSourceLabel(constraint);
    for (int i = 0; i < combo.count(); ++i) {
        const auto resolution = static_cast<livekit::ScreenShareResolution>(combo.itemData(i).toInt());
        const QString base = screenShareResolutionBaseLabel(resolution);
        if (constraint.has_cap() &&
            screenShareResolutionExceedsConstraint(resolution, maximumPixels, constraint)) {
            combo.setItemText(i, QCoreApplication::translate("MeetingUI", "%1 (capped at %2)").arg(base, capLabel));
            combo.setItemData(i, QCoreApplication::translate("MeetingUI",
                "Current encoder (%1) supports up to %2 at this frame rate; selecting this option will apply %2.")
                .arg(sourceLabel, capLabel), Qt::ToolTipRole);
        } else {
            combo.setItemText(i, base);
            combo.setItemData(i, QString(), Qt::ToolTipRole);
        }
    }
}

inline QString screenShareConstraintHintText(
        QSize maximumPixels,
        livekit::ScreenShareResolution selected,
        const livekit::ScreenShareQualityConstraint &constraint) {
    if (!constraint.has_cap()) return {};
    const QString sourceLabel = screenShareConstraintSourceLabel(constraint);
    const QString capLabel = screenShareResolutionShortLabel(*constraint.max_resolution);
    if (screenShareResolutionExceedsConstraint(selected, maximumPixels, constraint)) {
        return QCoreApplication::translate("MeetingUI",
            "Current encoder (%1) supports up to %2 at this frame rate. Actual sharing will be adjusted from %3 to %2.")
            .arg(sourceLabel, capLabel, screenShareResolutionShortLabel(selected));
    }
    const bool anyChoiceExceeds =
        screenShareResolutionExceedsConstraint(livekit::ScreenShareResolution::Native, maximumPixels, constraint) ||
        (screenShareResolutionFits(maximumPixels, QSize(3840, 2160)) &&
            screenShareResolutionExceedsConstraint(livekit::ScreenShareResolution::P2160, maximumPixels, constraint)) ||
        (screenShareResolutionFits(maximumPixels, QSize(2560, 1440)) &&
            screenShareResolutionExceedsConstraint(livekit::ScreenShareResolution::P1440, maximumPixels, constraint)) ||
        (screenShareResolutionFits(maximumPixels, QSize(1920, 1080)) &&
            screenShareResolutionExceedsConstraint(livekit::ScreenShareResolution::P1080, maximumPixels, constraint));
    if (!anyChoiceExceeds) return {};
    return QCoreApplication::translate("MeetingUI",
        "Current encoder (%1) supports up to %2 at this frame rate.")
        .arg(sourceLabel, capLabel);
}

inline QString screenShareDowngradeSummary(
        livekit::ScreenShareResolution requestedResolution,
        const livekit::ScreenShareQualityConstraint &constraint) {
    const QString requestedLabel = screenShareResolutionShortLabel(requestedResolution);
    const QString sourceLabel = screenShareConstraintSourceLabel(constraint);
    if (!sourceLabel.isEmpty()) {
        return QCoreApplication::translate("MeetingUI", " — Adjusted from %1 due to %2 limit")
            .arg(requestedLabel, sourceLabel);
    }
    return QCoreApplication::translate("MeetingUI", " — Adjusted from %1 to fit encoder limit")
        .arg(requestedLabel);
}

inline void selectScreenShareResolution(QComboBox &combo, int resolution) {
    const int index = combo.findData(resolution);
    combo.setCurrentIndex(index >= 0 ? index : combo.findData(static_cast<int>(livekit::ScreenShareResolution::Auto)));
}

} // namespace MeetingUI
