#pragma once

#include "whiteboard_canvas.h"
#include "src/media/screen_binding.h"

#include <QtCore/QPointer>
#include <QtWidgets/QWidget>

class QAbstractButton;
class QLabel;
class QPushButton;
class QScreen;

namespace MeetingUI {
class AnnotationOverlayWindow final : public QWidget {
    Q_OBJECT
public:
    explicit AnnotationOverlayWindow(livekit::ScreenBinding binding,
                                     bool showWindows = true);
    ~AnnotationOverlayWindow() override;

    const livekit::ScreenBinding &binding() const { return binding_; }
    WhiteboardCanvas *canvas() const { return canvas_; }
    bool desktopMode() const { return desktopMode_; }
    bool interactionEnabled() const { return interactionEnabled_; }
    QWidget *toolbar() const { return toolbar_; }

    void setDesktopMode(bool desktopMode);
    void setInteractionEnabled(bool enabled);
    void closeOverlay();

signals:
    void closeRequested();
    void bindingInvalidated();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, long *result) override;

private:
    QScreen *resolveScreen() const;
    void createToolbar(QScreen *screen, bool showWindows);
    void positionToolbar(QScreen *screen);
    void invalidateBinding();
    void exportLayer();
    void updateToolbarState();

    livekit::ScreenBinding binding_;
    livekit::whiteboard::Document document_;
    WhiteboardCanvas *canvas_ = nullptr;
    QWidget *toolbar_ = nullptr;
    QPushButton *drawMode_ = nullptr;
    QPushButton *export_ = nullptr;
    QPushButton *close_ = nullptr;
    QLabel *status_ = nullptr;
    QPointer<QScreen> screen_;
    bool desktopMode_ = false;
    bool interactionEnabled_ = true;
    bool invalidated_ = false;
};
}
