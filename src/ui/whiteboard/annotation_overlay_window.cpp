#include "annotation_overlay_window.h"

#include "src/platform/win/annotation_window_support.h"
#include "src/ui/app_theme.h"

#include <QtCore/QSaveFile>
#include <QtCore/QSignalBlocker>
#include <QtCore/QUuid>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QResizeEvent>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace MeetingUI {
namespace wb = livekit::whiteboard;
namespace {
QString normalizedDisplayName(QString name) {
    name = name.trimmed().toUpper();
    if (name.startsWith(QStringLiteral("\\\\.\\"))) name.remove(0, 4);
    return name;
}

QPushButton *toolButton(QHBoxLayout *layout, const QString &text, const char *name) {
    auto *button = new QPushButton(text);
    button->setObjectName(QString::fromLatin1(name));
    layout->addWidget(button);
    return button;
}
}

AnnotationOverlayWindow::AnnotationOverlayWindow(
        livekit::ScreenBinding binding, bool showWindows)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint)
    , binding_(std::move(binding))
    , document_("annotation-" + binding_.share_session_id, "local",
                binding_.canonical_width, binding_.canonical_height) {
    setObjectName(QStringLiteral("annotationOverlayWindow"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    screen_ = resolveScreen();
    if (!screen_) throw std::invalid_argument("annotation screen binding is unavailable");
    createWinId();
    if (windowHandle()) windowHandle()->setScreen(screen_);
    setGeometry(screen_->geometry());

    canvas_ = new WhiteboardCanvas(document_, this);
    canvas_->setOverlayMode(true);
    canvas_->installEventFilter(this);
    canvas_->setGeometry(rect());

    createToolbar(screen_, showWindows);
    const auto invalidate = [this] { invalidateBinding(); };
    connect(screen_, &QScreen::geometryChanged, this, invalidate);
    connect(screen_, &QScreen::logicalDotsPerInchChanged, this, invalidate);
    connect(screen_, &QScreen::orientationChanged, this, invalidate);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen *removed) {
        if (removed == screen_) invalidateBinding();
    });

    if (showWindows) {
        show();
        raise();
        toolbar_->show();
        toolbar_->raise();
    }
    setDesktopMode(false);
}

AnnotationOverlayWindow::~AnnotationOverlayWindow() {
    canvas_->cancelInput();
    if (toolbar_) {
        toolbar_->hide();
        delete toolbar_;
        toolbar_ = nullptr;
    }
}

QScreen *AnnotationOverlayWindow::resolveScreen() const {
    QScreen *result = nullptr;
    const auto wanted = normalizedDisplayName(QString::fromStdString(binding_.display_name));
    for (auto *candidate : QGuiApplication::screens()) {
        if (normalizedDisplayName(candidate->name()) != wanted) continue;
        if (result) return nullptr;
        result = candidate;
    }
    if (!result || binding_.physical_width <= 0 || binding_.physical_height <= 0) return nullptr;
    const auto geometry = result->geometry();
    if (geometry.width() <= 0 || geometry.height() <= 0) return nullptr;
    const double physicalRatio = static_cast<double>(binding_.physical_width) /
        binding_.physical_height;
    const double logicalRatio = static_cast<double>(geometry.width()) / geometry.height();
    return std::abs(physicalRatio - logicalRatio) <= 0.02 ? result : nullptr;
}

void AnnotationOverlayWindow::createToolbar(QScreen *screen, bool showWindows) {
    // Keep the controls in the overlay's native owner chain. Independent
    // TOPMOST windows can be reordered behind the full-screen input surface.
    toolbar_ = new QWidget(this,
        Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    toolbar_->setObjectName(QStringLiteral("annotationToolbar"));
    toolbar_->setAttribute(Qt::WA_StyledBackground);
    AppTheme::setTone(*toolbar_, AppTheme::Tone::Light);
    AppTheme::setStyleVariant(*toolbar_, "whiteboard-panel");
    auto *outer = new QVBoxLayout(toolbar_);
    outer->setContentsMargins(8, 6, 8, 6);
    outer->setSpacing(4);

    auto *tools = new QHBoxLayout;
    drawMode_ = toolButton(tools, tr("Draw"), "annotationDrawMode");
    drawMode_->setCheckable(true);
    drawMode_->setChecked(true);
    drawMode_->setToolTip(tr("Switch between drawing and operating the shared desktop"));
    connect(drawMode_, &QPushButton::toggled, this,
            [this](bool checked) { setDesktopMode(!checked); });

    const QStringList names{tr("Pen"), tr("Highlighter"), tr("Line"), tr("Rectangle"),
        tr("Ellipse"), tr("Arrow"), tr("Text"), tr("Eraser"), tr("Laser")};
    auto *group = new QButtonGroup(toolbar_);
    for (int index = 0; index < names.size(); ++index) {
        auto *button = toolButton(tools, names[index], "annotationTool");
        button->setCheckable(true);
        button->setChecked(index == 0);
        button->setProperty("toolId", index);
        group->addButton(button, index);
        connect(button, &QPushButton::clicked, this, [this, index] {
            canvas_->setTool(static_cast<WhiteboardCanvas::Tool>(index));
            setDesktopMode(false);
        });
    }
    outer->addLayout(tools);

    auto *options = new QHBoxLayout;
    auto *colors = new QComboBox(toolbar_);
    colors->setAccessibleName(tr("Ink color"));
    const std::pair<QString, QRgb> palette[]{{tr("Black"), 0x1f2329}, {tr("Red"), 0xf53f3f},
        {tr("Blue"), 0x1677ff}, {tr("Green"), 0x00a870}, {tr("Orange"), 0xff9f1a}};
    for (const auto &color : palette) {
        QPixmap swatch(14, 14);
        swatch.fill(QColor::fromRgb(color.second));
        colors->addItem(QIcon(swatch), color.first, color.second);
    }
    options->addWidget(colors);
    connect(colors, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, colors] {
        canvas_->setInkColor(QColor::fromRgb(colors->currentData().toUInt()));
    });
    options->addWidget(new QLabel(tr("Width"), toolbar_));
    auto *width = new QSpinBox(toolbar_);
    width->setRange(1, 32);
    width->setValue(4);
    options->addWidget(width);
    connect(width, qOverload<int>(&QSpinBox::valueChanged), canvas_, &WhiteboardCanvas::setInkWidth);
    options->addWidget(new QLabel(tr("Text size"), toolbar_));
    auto *font = new QSpinBox(toolbar_);
    font->setRange(8, 96);
    font->setValue(28);
    options->addWidget(font);
    connect(font, qOverload<int>(&QSpinBox::valueChanged), canvas_, &WhiteboardCanvas::setTextSize);
    auto *undo = toolButton(options, tr("Undo"), "annotationUndo");
    auto *redo = toolButton(options, tr("Redo"), "annotationRedo");
    auto *clear = toolButton(options, tr("Clear"), "annotationClear");
    connect(undo, &QPushButton::clicked, this, [this] { canvas_->execute(wb::CommandKind::Undo); });
    connect(redo, &QPushButton::clicked, this, [this] { canvas_->execute(wb::CommandKind::Redo); });
    connect(clear, &QPushButton::clicked, this, [this] {
        QMessageBox box(QMessageBox::Question, tr("Clear annotations"),
            tr("Remove all annotations from the shared screen?"),
            QMessageBox::Yes | QMessageBox::Cancel, toolbar_);
        box.setDefaultButton(QMessageBox::Cancel);
        box.setMinimumSize(460, 180);
        AppTheme::setTone(box, AppTheme::Tone::Light);
        if (box.exec() == QMessageBox::Yes) canvas_->execute(wb::CommandKind::Clear);
    });
    options->addStretch();
    status_ = new QLabel(toolbar_);
    status_->setObjectName(QStringLiteral("annotationStatus"));
    options->addWidget(status_);
    export_ = toolButton(options, tr("Export layer"), "annotationExport");
    close_ = toolButton(options, tr("Close annotations"), "annotationClose");
    connect(export_, &QPushButton::clicked, this, &AnnotationOverlayWindow::exportLayer);
    connect(close_, &QPushButton::clicked, this, [this] {
        closeOverlay();
        emit closeRequested();
    });
    outer->addLayout(options);
    AppTheme::styleChoiceControls(*toolbar_, AppTheme::Tone::Light);
    toolbar_->adjustSize();
    positionToolbar(screen);
    if (!showWindows) toolbar_->hide();
    updateToolbarState();
}

void AnnotationOverlayWindow::positionToolbar(QScreen *screen) {
    if (!toolbar_ || !screen) return;
    const auto target = screen->geometry();
    toolbar_->adjustSize();
    const int width = std::min(toolbar_->width(), std::max(320, target.width() - 24));
    toolbar_->resize(width, toolbar_->height());
    toolbar_->move(target.left() + (target.width() - width) / 2, target.top() + 12);
}

void AnnotationOverlayWindow::setDesktopMode(bool desktopMode) {
    desktopMode_ = desktopMode || !interactionEnabled_;
    canvas_->cancelInput();
    // Desktop mode controls native pointer routing, not the authorization of
    // toolbar commands such as undo, redo and clear.
    canvas_->setEditingEnabled(interactionEnabled_);
    canvas_->setOverlayInputSurface(interactionEnabled_ && !desktopMode_);
    Platform::ApplyAnnotationWindowMode(winId(), desktopMode_);
    if (drawMode_) {
        const QSignalBlocker blocker(drawMode_);
        drawMode_->setChecked(!desktopMode_ && interactionEnabled_);
    }
    updateToolbarState();
    if (!desktopMode_) canvas_->setFocus(Qt::OtherFocusReason);
    if (toolbar_ && toolbar_->isVisible()) toolbar_->raise();
}

void AnnotationOverlayWindow::setInteractionEnabled(bool enabled) {
    if (interactionEnabled_ == enabled) return;
    interactionEnabled_ = enabled;
    setDesktopMode(!enabled || desktopMode_);
    updateToolbarState();
}

void AnnotationOverlayWindow::updateToolbarState() {
    if (!toolbar_) return;
    for (auto *button : toolbar_->findChildren<QAbstractButton *>()) {
        if (button == close_ || button == export_) continue;
        button->setEnabled(interactionEnabled_);
    }
    for (auto *combo : toolbar_->findChildren<QComboBox *>()) combo->setEnabled(interactionEnabled_);
    for (auto *spin : toolbar_->findChildren<QSpinBox *>()) spin->setEnabled(interactionEnabled_);
    if (status_) status_->setText(interactionEnabled_
        ? (desktopMode_ ? tr("Desktop mode") : tr("Drawing on shared screen"))
        : tr("Paused while reconnecting"));
}

void AnnotationOverlayWindow::closeOverlay() {
    canvas_->cancelInput();
    setDesktopMode(true);
    hide();
    if (toolbar_) toolbar_->hide();
}

void AnnotationOverlayWindow::invalidateBinding() {
    if (invalidated_) return;
    invalidated_ = true;
    setInteractionEnabled(false);
    closeOverlay();
    emit bindingInvalidated();
}

void AnnotationOverlayWindow::exportLayer() {
    canvas_->cancelInput();
    QFileDialog dialog(toolbar_, tr("Export annotation layer"));
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setNameFilter(tr("PNG image (*.png)"));
    dialog.setDefaultSuffix(QStringLiteral("png"));
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
    QSaveFile output(dialog.selectedFiles().front());
    const auto image = canvas_->exportImage();
    if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG") || !output.commit())
        QMessageBox::warning(toolbar_, tr("Export annotation layer"), tr("Unable to export the annotation layer."));
}

void AnnotationOverlayWindow::resizeEvent(QResizeEvent *event) {
    canvas_->setGeometry(rect());
    QWidget::resizeEvent(event);
}

void AnnotationOverlayWindow::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        setDesktopMode(true);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool AnnotationOverlayWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == canvas_ && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) {
            setDesktopMode(true);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool AnnotationOverlayWindow::nativeEvent(
        const QByteArray &eventType, void *message, long *result) {
    if (Platform::HandleAnnotationNativeEvent(message, result, desktopMode_)) return true;
    return QWidget::nativeEvent(eventType, message, result);
}
}
