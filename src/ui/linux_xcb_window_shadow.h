#pragma once

#include <QtCore/QAbstractNativeEventFilter>

#include <functional>
#include <memory>

class QWindow;

namespace MeetingUI {

// UI-thread-only native support for an independent, override-redirect shadow.
// Painting, geometry and visibility remain owned by LinuxWindowFrame.
class LinuxXcbWindowShadow final : public QAbstractNativeEventFilter {
public:
    explicit LinuxXcbWindowShadow(std::function<void()> scheduleSync);
    ~LinuxXcbWindowShadow() override;

    LinuxXcbWindowShadow(const LinuxXcbWindowShadow &) = delete;
    LinuxXcbWindowShadow &operator=(const LinuxXcbWindowShadow &) = delete;

    // Both native windows must already exist. Returns false when the body is
    // unmapped, compositing is unavailable, or native setup could not complete.
    bool configure(QWindow &body, QWindow &shadow);
    bool bodyMapped() const;
    void reset();

    bool nativeEventFilter(const QByteArray &eventType, void *message, long *result) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace MeetingUI
