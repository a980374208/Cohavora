#pragma once

#include "whiteboard_renderer.h"
#include <QtCore/QTimer>
#include <QtGui/QTransform>
#include <QtWidgets/QWidget>
#include <functional>
#include <map>

class QPlainTextEdit;

namespace MeetingUI {
class WhiteboardCanvas final : public QWidget {
    Q_OBJECT
public:
    enum class Tool { Pen, Highlighter, Line, Rectangle, Ellipse, Arrow, Text, Eraser, Laser, Pan };
    explicit WhiteboardCanvas(livekit::whiteboard::Document &document, QWidget *parent = nullptr);
    const livekit::whiteboard::Document &document() const { return document_; }
    void setTool(Tool tool);
    Tool tool() const { return tool_; }
    void setInkColor(QColor color);
    void setInkWidth(double width);
    void setTextSize(double pixels);
    void setZoom(double zoom);
    double zoom() const { return zoom_; }
    void fitPage();
    void cancelInput();
    void setActor(std::string actor) { actor_ = std::move(actor); }
    void setEditingEnabled(bool enabled);
    void setOverlayMode(bool enabled);
    void setOverlayInputSurface(bool enabled);
    bool overlayMode() const { return overlayMode_; }
    void setCommandHandler(std::function<livekit::whiteboard::Result(
        const livekit::whiteboard::Command &)> handler) { commandHandler_ = std::move(handler); }
    void refreshFromDocument();
    void setBackgroundImage(std::string assetId, QImage image);
    bool hasBackgroundImage(const std::string &assetId) const;
    QImage currentBackgroundImage() const;
    qint64 backgroundCacheBytes() const { return backgroundCacheBytes_; }
    livekit::whiteboard::Result execute(livekit::whiteboard::CommandKind kind, const std::string &target = {});
    livekit::whiteboard::Result applyImagePage(const std::string &assetId,
                                               double width, double height,
                                               const std::string &pageId,
                                               bool replaceCurrent);
    QRectF contentRect() const;
    QPointF documentToView(QPointF point) const;
    std::optional<QPointF> viewToDocument(QPointF point) const;
    QImage exportImage() const;
    qsizetype cacheBytes() const { return cache_.sizeInBytes(); }
    QSize minimumSizeHint() const override { return {160, 120}; }

signals:
    void documentChanged();
    void zoomChanged(double value);
    void notice(const QString &message);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void hideEvent(QHideEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;

private:
    QTransform transform() const;
    void clampPan();
    void invalidateCache(QRectF area = {});
    void refreshItems();
    livekit::whiteboard::Command command(livekit::whiteboard::CommandKind kind) const;
    livekit::whiteboard::Result apply(const livekit::whiteboard::Command &command);
    void finishGesture();
    void eraseAt(QPointF point);
    void openTextEditor(QPointF point);
    void commitText();
    void paintCommitted(QPainter &painter, QRect clip);
    void touchCurrentBackground();

    struct BackgroundEntry {
        QImage image;
        std::uint64_t lastUsed = 0;
    };

    livekit::whiteboard::Document &document_;
    std::string actor_;
    bool editingEnabled_ = true;
    bool overlayMode_ = false, overlayInputSurface_ = false;
    std::function<livekit::whiteboard::Result(const livekit::whiteboard::Command &)> commandHandler_;
    Tool tool_ = Tool::Pen;
    QColor color_{0x1f, 0x23, 0x29};
    double inkWidth_ = 4, textSize_ = 28, zoom_ = 1;
    QPointF pan_, panStart_, lastMouse_;
    bool panning_ = false, erasing_ = false;
    std::optional<livekit::whiteboard::Command> gesture_;
    std::vector<WhiteboardRendering::Item> items_;
    QImage cache_;
    QRegion dirty_;
    std::map<std::string, BackgroundEntry> backgrounds_;
    qint64 backgroundCacheBytes_ = 0;
    std::uint64_t backgroundUse_ = 0;
    QWidget *textBox_ = nullptr;
    QPlainTextEdit *textEdit_ = nullptr;
    QTimer laserTimer_;
    std::optional<QPointF> laser_;
};
} // namespace MeetingUI
