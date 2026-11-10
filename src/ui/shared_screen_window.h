#pragma once

#include <QtCore/QAbstractNativeEventFilter>
#include <QtWidgets/QWidget>
#include "src/core/video_demand_types.h"
#include "render/share_view_transform.h"
#include "src/render/video_render_frame.h"

class QLabel;
class QComboBox;
class QPushButton;
class QScrollBar;
class QTimer;
class QMenu;
class QVBoxLayout;
class ParticipantWindowInspectionAccess;
namespace livekit::render { class VideoCanvas; }
namespace MeetingUI {
class SharedScreenFrameView;

// UI-owned viewing surface. It owns no Room, track, subscription or capture.
class SharedScreenWindow final : public QWidget, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    explicit SharedScreenWindow(bool gpu);
    ~SharedScreenWindow() override;
    livekit::DetachedShareViewport viewport() const;
    bool viewing() const;
    void setSharingName(const QString& name);
    void setStatus(const QString& text, bool clear = false);
    void setLocalShareControls(const QPushButton* quality, const QPushButton* annotation,
                              const QString& details);
    void setFrame(const QImage& image, livekit::render::VideoRenderFrame::Ptr frame);
    void setGpuFrame(livekit::render::VideoRenderFrame::Ptr frame);
    void useCpu();
    void clearFrame();
    void locate();
    bool usesGpu() const { return _gpu; }
    bool restoredGeometry() const { return _restoredGeometry; }
    void setZoom(double zoom);
    void toggleFullscreen();
    void fitWindowToFrame();
    void ensureOnScreen();
signals:
    void returnRequested();
    void viewportChanged();
    void rendererUnavailable();
    void shareQualityRequested();
    void annotationRequested();
protected:
    bool eventFilter(QObject*, QEvent*) override;
    bool event(QEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void moveEvent(QMoveEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void changeEvent(QEvent*) override;
    void closeEvent(QCloseEvent*) override;
    void paintEvent(QPaintEvent*) override;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    bool nativeEvent(const QByteArray&, void*, qintptr*) override;
    bool nativeEventFilter(const QByteArray&, void*, qintptr*) override;
#else
    bool nativeEvent(const QByteArray&, void*, long*) override;
    bool nativeEventFilter(const QByteArray&, void*, long*) override;
#endif
private:
    friend class ::ParticipantWindowInspectionAccess;
    void layoutCanvas();
    void updateTransform();
    void updateFrameGeometry(int width, int height, int rotation);
    void layoutToolbar();
    void setCompact(bool compact);
    void noteInteraction();
    void setOnTop(bool enabled);
    void savePreferences();
    void refreshTheme();
    void setupNativeWindow();
    int resizeHit(qintptr position) const;
    QWidget* _toolbar = nullptr;
    QVBoxLayout* _layout = nullptr;
    QComboBox* _scale = nullptr;
    QPushButton* _fullscreen = nullptr;
    QPushButton* _more = nullptr;
    QPushButton* _back = nullptr;
    QPushButton* _expand = nullptr;
    QPushButton* _shareQuality = nullptr;
    QPushButton* _annotation = nullptr;
    bool _localShareControlsVisible = false;
    QScrollBar* _horizontal = nullptr;
    QScrollBar* _vertical = nullptr;
    QTimer* _idleTimer = nullptr;
    QMenu* _menu = nullptr;
    ShareViewTransform _transform;
    QRect _normalGeometry;
    QRect _beforeFullscreen;
    bool _wasMaximized = false;
    bool _compact = false;
    bool _pinnedToolbar = false;
    bool _onTop = false;
    bool _restoredGeometry = false;
    bool _screenConnected = false;
    bool _updatingTransform = false;
    bool _panning = false;
    QPoint _panPosition;
    QLabel* _name = nullptr;
    QLabel* _status = nullptr;
    QWidget* _statusBar = nullptr;
    QLabel* _shareDetails = nullptr;
    SharedScreenFrameView* _view = nullptr;
    livekit::render::VideoCanvas* _canvas = nullptr;
    bool _gpu = false;
    bool _nativeFilter = false;
    quint64 _receivedFrames = 0;
};
} // namespace MeetingUI
