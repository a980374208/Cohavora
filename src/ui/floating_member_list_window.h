#pragma once

#include <QtCore/QAbstractNativeEventFilter>
#include <QtCore/QPoint>
#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include "src/core/video_demand_types.h"
#include "src/render/video_render_frame.h"
#include "src/ui/render/video_canvas.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class QLabel;
class QToolButton;
class QPainter;
class ParticipantWindowInspectionAccess;

namespace MeetingUI {

class FloatingListViewport;
class MeetingVideoPager;

struct FloatingMemberSlot {
    QString renderKey;
    QRect rect;
    bool isSpeaking = false;
    float audioLevel = 0.0f;
    bool hasVideo = false;
    livekit::render::VideoCanvas::DecorationPainter decorationPainter;
    livekit::render::VideoCanvas::ToolTipProvider toolTipProvider;
    QRect pinRect;
    std::function<void(QPainter&, const QRect&, bool hovered)> cpuPainter;
};

// UI-owned detached viewing surface for SpeakerLayout::Floating thumbnails.
// Owns no Room, Track, capture device or session state.
class FloatingMemberListWindow final : public QWidget, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    explicit FloatingMemberListWindow(bool gpu, bool persistPreferences = true);
    ~FloatingMemberListWindow() override;

    livekit::FloatingStripViewport viewport() const;
    bool viewing() const;
    bool usesGpu() const { return _gpu; }
    bool isCollapsed() const { return _collapsed; }
    bool isAlwaysOnTop() const { return _onTop; }
    bool restoredGeometry() const { return _restoredGeometry; }

    void setSlots(std::vector<FloatingMemberSlot> memberSlots, const QString& emptyHint = {});
    void setMemberCount(int count);
    void setPage(uint32_t page, uint32_t pageCount);
    void setCollapsed(bool collapsed);
    void setAlwaysOnTop(bool enabled);

    void updateGpuFrame(const std::string& renderKey, livekit::render::VideoRenderFrame::Ptr frame);
    void updateTilePresentation(const std::string& renderKey, bool hasVideo);
    void removeUser(const std::string& renderKey);
    void requestCpuRepaint();
    void useCpu();
    void clearFrames();

    void placeBeside(const QWidget* hostWindow);
    void ensureOnScreen();

signals:
    void dockRequested();
    void viewportChanged();
    void rendererUnavailable();
    void pageRequested(uint32_t page);
    void tileDoubleClicked(const QString& renderKey);
    void tileMenuRequested(const QString& renderKey, const QPoint& globalPos);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool event(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;
#else
    bool nativeEvent(const QByteArray& eventType, void* message, long* result) override;
    bool nativeEventFilter(const QByteArray& eventType, void* message, long* result) override;
#endif

private:
    friend class ::ParticipantWindowInspectionAccess;
    friend class FloatingListViewport;

    void updateChromeText();
    void updateContentGeometry();
    void layoutCanvas();
    void applyTopMostFlag();
    void refreshTheme();
    void savePreferences();
    void setupNativeWindow();
    bool isTitleDragArea(const QPoint& windowPos) const;
    QPoint menuAnchorGlobalPos(const QString& renderKey) const;
    int resizeHit(qintptr position) const;

    QWidget* _titleBar = nullptr;
    QLabel* _iconLabel = nullptr;
    QLabel* _titleLabel = nullptr;
    QToolButton* _onTopButton = nullptr;
    QToolButton* _collapseButton = nullptr;
    QToolButton* _dockButton = nullptr;
    FloatingListViewport* _viewport = nullptr;
    QLabel* _emptyLabel = nullptr;
    MeetingVideoPager* _pager = nullptr;
    livekit::render::VideoCanvas* _canvas = nullptr;

    std::vector<FloatingMemberSlot> _slots;
    std::vector<std::string> _canvasKeys;
    QString _emptyHint;
    int _memberCount = 0;
    uint32_t _page = 0;
    uint32_t _pageCount = 1;

    QRect _normalGeometry;
    int _expandedHeight = 520;
    bool _gpu = false;
    bool _persistPreferences = true;
    bool _collapsed = false;
    bool _onTop = true;
    bool _restoredGeometry = false;
    bool _screenConnected = false;
    bool _nativeFilter = false;
};

} // namespace MeetingUI
