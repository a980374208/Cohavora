#pragma once

#include <QtCore/QPoint>
#include <QtCore/QMargins>
#include <QtCore/QRect>
#include <QtGui/QImage>
#include <functional>

class QWidget;

namespace MeetingUI {

struct LinuxWindowFrameOptions {
    int radius = 12;
    int resizeMargin = 6;
    // Logical window coordinates. Empty keeps the window's own title controls.
    std::function<bool(QPoint)> captionHitTest;
};

// Call after setting the top-level flags and before the first native show.
// Native decorated windows are left to the desktop; only explicitly frameless
// windows receive this fallback for their custom title bars.
// The owner and all painting/input handling remain on the window's UI thread.
void installLinuxWindowFrame(QWidget &window, LinuxWindowFrameOptions options = {});

// Supplement QtWayland's client-side title bar when it supplies no shadow.
// Keeps native flags, title/resize handling and content unchanged. X11 and
// Wayland server-side decorations continue to use their desktop's shadow.
void installLinuxClientDecorationShadow(QWidget &window);

// A value-only snapshot of the existing frame, for local presentation probes.
// Reading this does not render, create a native surface or request activation.
struct LinuxWindowFrameDiagnostics {
    bool installed = false;
    bool isWayland = false;
    bool shadowVisible = false;
    bool active = false;
    bool nativeShadowConfigured = false;
    bool clientDecorationShadowFallback = false;
    int radius = 0;
    int shadowMargin = 0;
    QRect shadowGeometry;
    QMargins nativeFrameMargins;
    QImage shadowImage;
    quint64 shadowRevision = 0;
    quint64 moveRequests = 0;
    quint64 resizeRequests = 0;
    bool lastMoveAccepted = false;
    bool lastResizeAccepted = false;
    Qt::Edges lastResizeEdges;
};
LinuxWindowFrameDiagnostics inspectLinuxWindowFrame(const QWidget &window);

} // namespace MeetingUI
