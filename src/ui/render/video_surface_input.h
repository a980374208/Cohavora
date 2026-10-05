#pragma once
#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtWidgets/QWidget>

namespace livekit::render {
// Native GPU surfaces and their QWidget host have the same local geometry.
// Keep the original event (including scan code, extended bit and acceptance)
// so the host's application event filter sees the same input as the CPU path.
inline bool ForwardVideoSurfaceInput(QWidget* host, QEvent* event) {
    if (!host) return false;
    switch (event->type()) {
    case QEvent::MouseMove: case QEvent::MouseButtonPress: case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick: case QEvent::Leave:
    case QEvent::KeyPress: case QEvent::KeyRelease: case QEvent::ShortcutOverride:
    case QEvent::Wheel:
        QCoreApplication::sendEvent(host, event);
        return true;
    default:
        return false;
    }
}
} // namespace livekit::render
