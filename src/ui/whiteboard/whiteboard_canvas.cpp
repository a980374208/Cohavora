#include "whiteboard_canvas.h"
#include "src/ui/app_theme.h"
#include <QtCore/QThread>
#include <QtCore/QUuid>
#include <QtCore/QtMath>
#include <QtCore/QSignalBlocker>
#include <QtGui/QInputMethod>
#include <QtGui/QHideEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPaintEvent>
#include <QtGui/QPainter>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QApplication>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace MeetingUI {
namespace wb = livekit::whiteboard;
namespace {
constexpr qint64 MaxBackgroundCacheBytes = 96LL * 1024 * 1024;
std::string newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(); }
QPointF point(const wb::Point &p) { return {p.x, p.y}; }
}

WhiteboardCanvas::WhiteboardCanvas(wb::Document &document, QWidget *parent)
    : QWidget(parent), document_(document), actor_(document.owner()) {
    setObjectName(QStringLiteral("whiteboardCanvas"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setCursor(Qt::CrossCursor);
    laserTimer_.setSingleShot(true);
    connect(&laserTimer_, &QTimer::timeout, this, [this] { laser_.reset(); update(); });
    refreshItems();
}

void WhiteboardCanvas::setEditingEnabled(bool enabled) {
    if (editingEnabled_ == enabled) return;
    editingEnabled_ = enabled;
    if (!enabled) cancelInput();
}

void WhiteboardCanvas::setOverlayMode(bool enabled) {
    if (overlayMode_ == enabled) return;
    cancelInput();
    overlayMode_ = enabled;
    if (enabled) {
        zoom_ = 1;
        pan_ = {};
        setAttribute(Qt::WA_OpaquePaintEvent, false);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAutoFillBackground(false);
    } else {
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setAttribute(Qt::WA_NoSystemBackground, false);
    }
    cache_ = {};
    invalidateCache();
}

void WhiteboardCanvas::setOverlayInputSurface(bool enabled) {
    if (overlayInputSurface_ == enabled) return;
    overlayInputSurface_ = enabled;
    update();
}

void WhiteboardCanvas::refreshFromDocument() {
    Q_ASSERT(QThread::currentThread() == thread());
    cancelInput();
    touchCurrentBackground();
    refreshItems();
}

void WhiteboardCanvas::setBackgroundImage(std::string assetId, QImage image) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (assetId.empty() || image.isNull() || image.sizeInBytes() > MaxBackgroundCacheBytes) return;
    if (auto existing = backgrounds_.find(assetId); existing != backgrounds_.end()) {
        backgroundCacheBytes_ -= existing->second.image.sizeInBytes();
        backgrounds_.erase(existing);
    }
    backgroundCacheBytes_ += image.sizeInBytes();
    backgrounds_.emplace(std::move(assetId), BackgroundEntry{std::move(image), ++backgroundUse_});
    const auto &current = document_.page().backgroundAssetId;
    while (backgroundCacheBytes_ > MaxBackgroundCacheBytes && backgrounds_.size() > 1) {
        auto oldest = backgrounds_.end();
        for (auto it = backgrounds_.begin(); it != backgrounds_.end(); ++it) {
            if (it->first == current) continue;
            if (oldest == backgrounds_.end() || it->second.lastUsed < oldest->second.lastUsed) oldest = it;
        }
        if (oldest == backgrounds_.end()) break;
        backgroundCacheBytes_ -= oldest->second.image.sizeInBytes();
        backgrounds_.erase(oldest);
    }
    invalidateCache();
}

bool WhiteboardCanvas::hasBackgroundImage(const std::string &assetId) const {
    return assetId.empty() || backgrounds_.count(assetId) != 0;
}

QImage WhiteboardCanvas::currentBackgroundImage() const {
    const auto found = backgrounds_.find(document_.page().backgroundAssetId);
    return found == backgrounds_.end() ? QImage{} : found->second.image;
}

void WhiteboardCanvas::touchCurrentBackground() {
    const auto found = backgrounds_.find(document_.page().backgroundAssetId);
    if (found != backgrounds_.end()) found->second.lastUsed = ++backgroundUse_;
}

QTransform WhiteboardCanvas::transform() const {
    const auto &page = document_.page();
    if (overlayMode_) {
        QTransform result;
        result.scale(std::max(1, width()) / page.width,
                     std::max(1, height()) / page.height);
        return result;
    }
    const double fit = std::max(0.001, std::min(std::max(1, width() - 32) / page.width,
                                             std::max(1, height() - 32) / page.height));
    const double scale = fit * zoom_;
    QTransform result;
    result.translate((width() - page.width * scale) / 2 + pan_.x(),
                     (height() - page.height * scale) / 2 + pan_.y());
    result.scale(scale, scale);
    return result;
}
QRectF WhiteboardCanvas::contentRect() const {
    return transform().mapRect(QRectF(0, 0, document_.page().width, document_.page().height));
}
QPointF WhiteboardCanvas::documentToView(QPointF p) const { return transform().map(p); }
std::optional<QPointF> WhiteboardCanvas::viewToDocument(QPointF p) const {
    if (!contentRect().contains(p)) return std::nullopt;
    return transform().inverted().map(p);
}

void WhiteboardCanvas::setTool(Tool tool) {
    cancelInput();
    tool_ = tool;
    setCursor(tool == Tool::Pan ? Qt::OpenHandCursor : tool == Tool::Text ? Qt::IBeamCursor : Qt::CrossCursor);
}
void WhiteboardCanvas::setInkColor(QColor color) { cancelInput(); if (color.isValid()) color_ = color; }
void WhiteboardCanvas::setInkWidth(double width) { cancelInput(); inkWidth_ = std::clamp(width, 1.0, 32.0); }
void WhiteboardCanvas::setTextSize(double pixels) { cancelInput(); textSize_ = std::clamp(pixels, 8.0, 96.0); }
void WhiteboardCanvas::setZoom(double zoom) {
    if (overlayMode_) return;
    cancelInput();
    zoom_ = std::clamp(zoom, 0.5, 2.0);
    clampPan();
    invalidateCache();
    emit zoomChanged(zoom_);
}
void WhiteboardCanvas::fitPage() {
    if (overlayMode_) return;
    pan_ = {};
    setZoom(1);
}
void WhiteboardCanvas::clampPan() {
    const auto scaled = transform().mapRect(QRectF(0, 0, document_.page().width, document_.page().height)).size();
    const double x = std::max(0.0, (scaled.width() - width() + 32) / 2);
    const double y = std::max(0.0, (scaled.height() - height() + 32) / 2);
    pan_.setX(std::clamp(pan_.x(), -x, x));
    pan_.setY(std::clamp(pan_.y(), -y, y));
}

void WhiteboardCanvas::cancelInput() {
    gesture_.reset();
    panning_ = erasing_ = false;
    laserTimer_.stop();
    laser_.reset();
    if (textBox_) {
        if (textEdit_) {
            QGuiApplication::inputMethod()->reset();
            textEdit_->removeEventFilter(this);
        }
        for (auto *child : textBox_->findChildren<QObject *>()) disconnect(child, nullptr, this, nullptr);
        textBox_->hide();
        textBox_->deleteLater();
        textBox_ = nullptr;
        textEdit_ = nullptr;
    }
    update();
}
wb::Command WhiteboardCanvas::command(wb::CommandKind kind) const {
    wb::Command value;
    value.id = newId();
    value.actor = actor_;
    value.context = document_.context();
    value.kind = kind;
    return value;
}
wb::Result WhiteboardCanvas::execute(wb::CommandKind kind, const std::string &target) {
    cancelInput();
    auto value = command(kind);
    value.target = target;
    if ((kind == wb::CommandKind::Redo || kind == wb::CommandKind::AddPage) && value.target.empty())
        value.target = newId();
    const auto result = apply(value);
    if (result.changed() && (kind == wb::CommandKind::SelectPage || kind == wb::CommandKind::AddPage)) fitPage();
    return result;
}
wb::Result WhiteboardCanvas::applyImagePage(const std::string &assetId,
                                             double width, double height,
                                             const std::string &pageId,
                                             bool replaceCurrent) {
    cancelInput();
    auto value = command(replaceCurrent ? wb::CommandKind::SetBackground : wb::CommandKind::AddImagePage);
    value.assetId = assetId;
    value.pageWidth = width;
    value.pageHeight = height;
    value.target = pageId;
    const auto result = apply(value);
    if (result.changed()) fitPage();
    return result;
}
wb::Result WhiteboardCanvas::apply(const wb::Command &value) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!editingEnabled_) return {wb::Status::Forbidden, {}};
    const auto result = commandHandler_ ? commandHandler_(value) : document_.apply(value);
    if (result.changed()) {
        if (value.kind == wb::CommandKind::Add || value.kind == wb::CommandKind::Redo) {
            items_.push_back(WhiteboardRendering::compile(document_.page().objects.back()));
            invalidateCache(transform().mapRect(items_.back().bounds));
        } else if (value.kind == wb::CommandKind::Erase || value.kind == wb::CommandKind::Undo) {
            const auto it = std::find_if(items_.begin(), items_.end(), [&](const auto &item) { return item.id == result.objectId; });
            if (it != items_.end()) {
                const auto bounds = transform().mapRect(it->bounds);
                items_.erase(it);
                invalidateCache(bounds);
            }
        } else refreshItems();
        emit notice({});
        emit documentChanged();
    } else if (result.status == wb::Status::LimitReached) {
        emit notice(tr("Whiteboard limit reached. Export this page or clear some content."));
    } else if (result.status == wb::Status::Invalid) {
        emit notice(tr("This object is invalid or too large. Shorten it and try again."));
    } else if (result.status == wb::Status::Stale) {
        emit notice(tr("The page changed. The unfinished input was cancelled."));
    }
    return result;
}
void WhiteboardCanvas::refreshItems() {
    items_.clear();
    for (const auto &object : document_.page().objects) items_.push_back(WhiteboardRendering::compile(object));
    invalidateCache();
}
void WhiteboardCanvas::invalidateCache(QRectF area) {
    const QRect dirty = area.isEmpty() ? rect() : area.toAlignedRect().adjusted(-3, -3, 3, 3).intersected(rect());
    dirty_ += dirty;
    update(dirty);
}
void WhiteboardCanvas::paintCommitted(QPainter &painter, QRect clip) {
    painter.save();
    painter.setClipRect(clip);
    if (overlayMode_) {
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(clip, Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    } else {
        painter.fillRect(clip, QColor(0xed, 0xf0, 0xf5));
    }
    painter.setTransform(transform(), true);
    const QRectF pageRect(0, 0, document_.page().width, document_.page().height);
    if (!overlayMode_) painter.fillRect(pageRect, Qt::white);
    painter.setClipRect(pageRect, Qt::IntersectClip);
    const auto background = currentBackgroundImage();
    if (!overlayMode_ && !background.isNull()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(pageRect, background);
    }
    const auto documentClip = transform().inverted().mapRect(QRectF(clip));
    for (const auto &item : items_)
        if (item.bounds.intersects(documentClip)) WhiteboardRendering::draw(painter, item);
    painter.restore();
}
void WhiteboardCanvas::paintEvent(QPaintEvent *event) {
    const double dpr = devicePixelRatioF();
    const QSize pixels(qCeil(width() * dpr), qCeil(height() * dpr));
    const auto required = static_cast<qint64>(pixels.width()) * pixels.height() * 4;
    if (cache_.size() != pixels || cache_.devicePixelRatio() != dpr) {
        cache_ = required > 0 && required <= 64 * 1024 * 1024
            ? QImage(pixels, QImage::Format_ARGB32_Premultiplied) : QImage();
        cache_.setDevicePixelRatio(dpr);
        dirty_ = rect();
    }
    QPainter painter(this);
    if (overlayMode_ && overlayInputSurface_)
        painter.fillRect(rect(), QColor(0, 0, 0, 1));
    if (!cache_.isNull()) {
        if (!dirty_.isEmpty()) {
            QPainter cached(&cache_);
            for (const auto &region : dirty_) paintCommitted(cached, region);
            dirty_ = {};
        }
        painter.drawImage(QPoint(0, 0), cache_);
    } else paintCommitted(painter, event->rect());
    painter.setTransform(transform());
    painter.setClipRect(QRectF(0, 0, document_.page().width, document_.page().height));
    if (gesture_ && tool_ != Tool::Text) WhiteboardRendering::draw(painter, WhiteboardRendering::compile(gesture_->object));
    if (laser_) {
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(245, 63, 63, 180));
        painter.drawEllipse(*laser_, 10 / transform().m11(), 10 / transform().m11());
    }
}
void WhiteboardCanvas::resizeEvent(QResizeEvent *event) {
    cancelInput();
    clampPan();
    cache_ = {};
    invalidateCache();
    QWidget::resizeEvent(event);
}
void WhiteboardCanvas::hideEvent(QHideEvent *event) {
    cancelInput();
    cache_ = {};
    QWidget::hideEvent(event);
}

void WhiteboardCanvas::mousePressEvent(QMouseEvent *event) {
    if (textBox_) return;
    if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && tool_ == Tool::Pan)) {
        cancelInput();
        panning_ = true;
        panStart_ = pan_;
        lastMouse_ = event->localPos();
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) return;
    if (!editingEnabled_ && tool_ != Tool::Laser) return;
    const auto position = viewToDocument(event->localPos());
    if (!position) return;
    setFocus(Qt::MouseFocusReason);
    if (tool_ == Tool::Eraser) { erasing_ = true; eraseAt(*position); return; }
    if (tool_ == Tool::Laser) {
        laser_ = position;
        laserTimer_.start(800);
        update();
        return;
    }
    if (tool_ == Tool::Text) { openTextEditor(*position); return; }
    gesture_ = command(wb::CommandKind::Add);
    auto &object = gesture_->object;
    object.id = newId();
    object.author = actor_;
    object.kind = static_cast<wb::ObjectKind>(tool_);
    object.color = color_.rgb() & 0xffffff;
    object.width = tool_ == Tool::Highlighter ? std::min(64.0, inkWidth_ * 4) : inkWidth_;
    object.points.push_back({position->x(), position->y()});
    if (tool_ != Tool::Pen && tool_ != Tool::Highlighter) object.points.push_back(object.points.front());
    update(transform().mapRect(WhiteboardRendering::compile(object).bounds).toAlignedRect().adjusted(-3, -3, 3, 3));
}
void WhiteboardCanvas::mouseMoveEvent(QMouseEvent *event) {
    if (textBox_) return;
    if (panning_) {
        pan_ = panStart_ + event->localPos() - lastMouse_;
        clampPan();
        invalidateCache();
        return;
    }
    const auto position = viewToDocument(event->localPos());
    if (tool_ == Tool::Laser && position && event->buttons().testFlag(Qt::LeftButton)) {
        laser_ = position;
        laserTimer_.start(800);
        update();
    }
    if (erasing_ && position) eraseAt(*position);
    if (!gesture_) return;
    if (!position) { finishGesture(); return; }
    auto &object = gesture_->object;
    const auto previous = point(object.points.back());
    QRectF dirty;
    if (object.kind == wb::ObjectKind::Pen || object.kind == wb::ObjectKind::Highlighter) {
        if (QLineF(previous, *position).length() * transform().m11() < 1.5) return;
        object.points.push_back({position->x(), position->y()});
        const double margin = object.width / 2 + 2;
        dirty = QRectF(previous, *position).normalized().adjusted(-margin, -margin, margin, margin);
    } else {
        dirty = WhiteboardRendering::compile(object).bounds;
        object.points.back() = {position->x(), position->y()};
        dirty = dirty.united(WhiteboardRendering::compile(object).bounds);
    }
    update(transform().mapRect(dirty).toAlignedRect().adjusted(-3, -3, 3, 3));
    if (object.points.size() >= wb::Document::MaxStrokePoints) {
        finishGesture();
        emit notice(tr("This stroke is full. Release the mouse and start a new stroke."));
    }
}
void WhiteboardCanvas::mouseReleaseEvent(QMouseEvent *event) {
    if (textBox_) return;
    if (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton) {
        if (gesture_) mouseMoveEvent(event);
        finishGesture();
        panning_ = erasing_ = false;
    }
}
void WhiteboardCanvas::finishGesture() {
    if (!gesture_ || textBox_) return;
    const auto value = std::move(*gesture_);
    gesture_.reset();
    const auto dirty = transform().mapRect(WhiteboardRendering::compile(value.object).bounds).toAlignedRect().adjusted(-3, -3, 3, 3);
    apply(value);
    update(dirty);
}
void WhiteboardCanvas::eraseAt(QPointF position) {
    const auto tolerance = 6 / transform().m11();
    for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
        if (!WhiteboardRendering::hit(*it, position, tolerance)) continue;
        auto value = command(wb::CommandKind::Erase);
        value.target = it->id;
        apply(value);
        break;
    }
}
void WhiteboardCanvas::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) { cancelInput(); event->accept(); }
    else if (editingEnabled_ && event->matches(QKeySequence::Undo)) { execute(wb::CommandKind::Undo); event->accept(); }
    else if (editingEnabled_ && event->matches(QKeySequence::Redo)) { execute(wb::CommandKind::Redo); event->accept(); }
    else QWidget::keyPressEvent(event);
}
void WhiteboardCanvas::wheelEvent(QWheelEvent *event) {
    if (overlayMode_) { event->ignore(); return; }
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        setZoom(zoom_ + (event->angleDelta().y() > 0 ? 0.1 : -0.1));
        event->accept();
    } else QWidget::wheelEvent(event);
}

void WhiteboardCanvas::openTextEditor(QPointF position) {
    cancelInput();
    const auto &page = document_.page();
    const QPointF end(std::min(page.width, position.x() + 480), std::min(page.height, position.y() + 200));
    if (end.x() - position.x() < textSize_ || end.y() - position.y() < textSize_ * 1.5) {
        emit notice(tr("Choose a text position farther from the page edge."));
        return;
    }
    gesture_ = command(wb::CommandKind::Add);
    auto &object = gesture_->object;
    object.id = newId();
    object.author = document_.owner();
    object.kind = wb::ObjectKind::Text;
    object.color = color_.rgb() & 0xffffff;
    object.fontSize = textSize_;
    object.points = {{position.x(), position.y()}, {end.x(), end.y()}};
    textBox_ = new QWidget(this);
    textBox_->setObjectName(QStringLiteral("whiteboardTextBox"));
    AppTheme::setTone(*textBox_, AppTheme::Tone::Light);
    auto *layout = new QVBoxLayout(textBox_);
    layout->setContentsMargins(8, 8, 8, 8);
    textEdit_ = new QPlainTextEdit(textBox_);
    textEdit_->setObjectName(QStringLiteral("whiteboardTextEditor"));
    textEdit_->setPlaceholderText(tr("Enter text (Ctrl+Enter to apply, Esc to cancel)"));
    textEdit_->installEventFilter(this);
    textEdit_->setTabChangesFocus(true);
    connect(textEdit_, &QPlainTextEdit::textChanged, this, [this] {
        // Bound retained text, including paste. Preedit remains owned by Qt/IME.
        const auto characters = textEdit_->toPlainText().toUcs4();
        if (characters.size() <= 2000) return;
        const QSignalBlocker blocker(textEdit_);
        textEdit_->setPlainText(QString::fromUcs4(characters.constData(), 2000));
        textEdit_->moveCursor(QTextCursor::End);
        emit notice(tr("Text is limited to 2,000 characters."));
    });
    layout->addWidget(textEdit_);
    auto *buttons = new QHBoxLayout;
    auto *save = new QPushButton(tr("Apply text"), textBox_);
    save->setObjectName(QStringLiteral("whiteboardApplyText"));
    auto *cancel = new QPushButton(tr("Cancel"), textBox_);
    buttons->addWidget(save);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);
    connect(save, &QPushButton::clicked, this, &WhiteboardCanvas::commitText);
    connect(cancel, &QPushButton::clicked, this, &WhiteboardCanvas::cancelInput);
    const QSize size(std::min(360, width()), std::min(200, height()));
    const auto anchor = documentToView(position);
    textBox_->setGeometry(std::clamp(qRound(anchor.x()), 0, std::max(0, width() - size.width())),
                          std::clamp(qRound(anchor.y()), 0, std::max(0, height() - size.height())), size.width(), size.height());
    textBox_->show();
    textBox_->raise();
    textEdit_->setFocus(Qt::OtherFocusReason);
}
void WhiteboardCanvas::commitText() {
    if (!gesture_ || !textEdit_) return;
    QGuiApplication::inputMethod()->commit();
    const auto text = textEdit_->toPlainText().trimmed();
    if (text.isEmpty()) { cancelInput(); return; }
    if (text.toUcs4().size() > 2000) { emit notice(tr("Text is limited to 2,000 characters.")); return; }
    auto value = *gesture_;
    value.object.text = text.toUtf8().toStdString();
    cancelInput();
    apply(value);
    setFocus(Qt::OtherFocusReason);
}
bool WhiteboardCanvas::eventFilter(QObject *watched, QEvent *event) {
    if (watched == textEdit_ && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) { cancelInput(); return true; }
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && key->modifiers().testFlag(Qt::ControlModifier)) {
            commitText();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
QImage WhiteboardCanvas::exportImage() const {
    return WhiteboardRendering::renderPage(document_.page(), currentBackgroundImage(), overlayMode_);
}
} // namespace MeetingUI
