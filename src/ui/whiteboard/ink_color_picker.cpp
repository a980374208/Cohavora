#include "ink_color_picker.h"

#include "src/ui/app_theme.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QRegularExpression>
#include <QtCore/QScopedValueRollback>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QAccessible>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QScreen>
#include <QtGui/QStandardItemModel>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListView>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QShortcut>
#include <QtWidgets/QSlider>
#include <QtWidgets/QStyledItemDelegate>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <array>
#include <functional>
#include <utility>

namespace MeetingUI {
namespace {
constexpr int Columns = 6;
constexpr int SwatchSize = 28;
constexpr int Gap = 6;
constexpr auto ColorRole = Qt::UserRole + 1;
constexpr auto Presets = std::array{
    std::pair{0x1f2329, QT_TRANSLATE_NOOP("MeetingUI::InkColorPicker", "Black")},
    std::pair{0xf53f3f, QT_TRANSLATE_NOOP("MeetingUI::InkColorPicker", "Red")},
    std::pair{0x1677ff, QT_TRANSLATE_NOOP("MeetingUI::InkColorPicker", "Blue")},
    std::pair{0x00a870, QT_TRANSLATE_NOOP("MeetingUI::InkColorPicker", "Green")},
    std::pair{0xff9f1a, QT_TRANSLATE_NOOP("MeetingUI::InkColorPicker", "Orange")},
    std::pair{0x722ed1, QT_TRANSLATE_NOOP("MeetingUI::InkColorPicker", "Purple")},
    std::pair{0xffffff, QT_TRANSLATE_NOOP("MeetingUI::InkColorPicker", "White")},
    std::pair{0x86909c, QT_TRANSLATE_NOOP("MeetingUI::InkColorPicker", "Gray")},
    std::pair{0xffd43b, QT_TRANSLATE_NOOP("MeetingUI::InkColorPicker", "Yellow")},
    std::pair{0x13c2c2, QT_TRANSLATE_NOOP("MeetingUI::InkColorPicker", "Cyan")},
    std::pair{0xf759ab, QT_TRANSLATE_NOOP("MeetingUI::InkColorPicker", "Pink")},
    std::pair{0x1d39c4, QT_TRANSLATE_NOOP("MeetingUI::InkColorPicker", "Navy")},
};

QString hex(QColor color) {
    return color.name(QColor::HexRgb).toUpper();
}

QString colorName(QColor color) {
    for (const auto &[value, name] : Presets) {
        if (QColor::fromRgb(value) == color) {
            return QStringLiteral("%1 (%2)")
                .arg(QCoreApplication::translate("MeetingUI::InkColorPicker", name), hex(color));
        }
    }
    return hex(color);
}

QList<QColor> presetColors(int count) {
    QList<QColor> colors;
    for (int index = 0; index < count; ++index) colors.append(QColor::fromRgb(Presets[index].first));
    return colors;
}

void drawSwatch(QPainter &painter, const QRectF &rect, QColor color) {
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QColor(0, 0, 0, 100));
    painter.setBrush(color);
    painter.drawRoundedRect(rect, 4, 4);
}

class SwatchDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override {
        return {SwatchSize, SwatchSize};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        painter->save();
        if (!option.state.testFlag(QStyle::State_Enabled)) painter->setOpacity(0.45);
        const auto rect = QRectF(option.rect).adjusted(4, 4, -4, -4);
        const auto color = index.data(ColorRole).value<QColor>();
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool focused = option.state.testFlag(QStyle::State_HasFocus);
        if (selected || focused || option.state.testFlag(QStyle::State_MouseOver)) {
            painter->setRenderHint(QPainter::Antialiasing);
            QPen outline(AppTheme::color(focused || selected ? AppTheme::ColorRole::Accent
                                                             : AppTheme::ColorRole::TextSecondary));
            if (focused && !selected) outline.setStyle(Qt::DashLine);
            painter->setPen(outline);
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(rect.adjusted(-2, -2, 2, 2), 5, 5);
        }
        drawSwatch(*painter, rect, color);
        if (selected) {
            const auto center = rect.center();
            const int luma = 299 * color.red() + 587 * color.green() + 114 * color.blue();
            painter->setPen(QPen(luma > 145000 ? Qt::black : Qt::white, 2,
                                 Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const QPointF tick[]{center + QPointF(-4, 0), center + QPointF(-1, 3),
                                 center + QPointF(5, -3)};
            painter->drawPolyline(tick, 3);
        }
        painter->restore();
    }
};

class ColorPlane final : public QWidget {
public:
    explicit ColorPlane(QWidget *parent) : QWidget(parent) {
        setObjectName(QStringLiteral("inkColorPlane"));
        setAccessibleName(InkColorPicker::tr("Saturation and brightness"));
        setAccessibleDescription(InkColorPicker::tr("Use the arrow keys to adjust the color."));
        setCursor(Qt::CrossCursor);
        setFocusPolicy(Qt::StrongFocus);
        setFixedHeight(96);
    }

    std::function<void(QColor)> onChange;
    int hue() const { return hue_; }

    void setColor(QColor color) {
        if (color.hsvHue() >= 0) hue_ = color.hsvHue();
        saturation_ = color.hsvSaturationF();
        value_ = color.valueF();
        update();
    }

    void setHue(int hue) {
        hue_ = hue;
        notifyColor();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        const auto bounds = QRectF(rect()).adjusted(1, 1, -1, -1);
        QPainterPath shape;
        shape.addRoundedRect(bounds, 5, 5);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.save();
        painter.setClipPath(shape);
        QLinearGradient saturation(bounds.topLeft(), bounds.topRight());
        saturation.setColorAt(0, Qt::white);
        saturation.setColorAt(1, QColor::fromHsv(hue_, 255, 255));
        painter.fillRect(bounds, saturation);
        QLinearGradient brightness(bounds.topLeft(), bounds.bottomLeft());
        brightness.setColorAt(0, QColor(0, 0, 0, 0));
        brightness.setColorAt(1, Qt::black);
        painter.fillRect(bounds, brightness);
        painter.restore();

        const QPointF cursor(5 + saturation_ * (width() - 10), 5 + (1 - value_) * (height() - 10));
        painter.setPen(QPen(Qt::black, 3));
        painter.drawEllipse(cursor, 4, 4);
        painter.setPen(QPen(Qt::white, 1.5));
        painter.drawEllipse(cursor, 4, 4);
        painter.setPen(AppTheme::color(hasFocus() ? AppTheme::ColorRole::Accent
                                                  : AppTheme::ColorRole::Border));
        painter.drawPath(shape);
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) selectAt(event->localPos());
        else QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (event->buttons().testFlag(Qt::LeftButton)) selectAt(event->localPos());
        else QWidget::mouseMoveEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override {
        constexpr qreal Step = 0.02;
        switch (event->key()) {
        case Qt::Key_Left: saturation_ -= Step; break;
        case Qt::Key_Right: saturation_ += Step; break;
        case Qt::Key_Down: value_ -= Step; break;
        case Qt::Key_Up: value_ += Step; break;
        default: QWidget::keyPressEvent(event); return;
        }
        saturation_ = std::clamp(saturation_, 0.0, 1.0);
        value_ = std::clamp(value_, 0.0, 1.0);
        notifyColor();
        event->accept();
    }

private:
    void selectAt(QPointF position) {
        saturation_ = std::clamp((position.x() - 5) / std::max(1, width() - 10), 0.0, 1.0);
        value_ = 1 - std::clamp((position.y() - 5) / std::max(1, height() - 10), 0.0, 1.0);
        notifyColor();
    }

    void notifyColor() {
        update();
        onChange(QColor::fromRgb(QColor::fromHsvF(hue_ / 360.0, saturation_, value_).rgb()));
    }

    int hue_ = 0;
    qreal saturation_ = 0;
    qreal value_ = 0;
};
} // namespace

class InkSwatchView final : public QListView {
public:
    InkSwatchView(int rows, const QString &name, QWidget *parent)
        : QListView(parent), colors_(new QStandardItemModel(this)) {
        setAccessibleName(name);
        setViewMode(QListView::IconMode);
        setMovement(QListView::Static);
        setResizeMode(QListView::Adjust);
        setWrapping(true);
        setSpacing(0);
        setGridSize({SwatchSize, SwatchSize});
        // QListView tests cell ends against the viewport's inclusive edge.
        // Leave room after the last cell so it stays on the intended row.
        setFixedSize(Columns * SwatchSize + 2, rows * SwatchSize + 2);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setMouseTracking(true);
        viewport()->setCursor(Qt::PointingHandCursor);
        setItemDelegate(new SwatchDelegate(this));
        setModel(colors_);
        connect(this, &QListView::clicked, this, [this](const QModelIndex &index) { activate(index); });
        connect(selectionModel(), &QItemSelectionModel::selectionChanged, this,
                [this](const QItemSelection &selected) {
            // Qt 5 accessibility selects through the model. Commit after the provider
            // returns, since choosing a recent color can reorder its own list.
            if (projecting_ || selected.isEmpty()) return;
            const QPersistentModelIndex index(selected.indexes().constFirst());
            QTimer::singleShot(0, this, [this, index] {
                if (index.isValid() && selectionModel()->isSelected(index)) activate(index);
            });
        });
    }

    std::function<void(QColor)> onSelect;

    void setColors(const QList<QColor> &colors) {
        colors_->removeRows(0, colors_->rowCount());
        for (const auto &color : colors) {
            auto *item = new QStandardItem(colorName(color));
            item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
            item->setAccessibleText(item->text());
            item->setToolTip(item->text());
            item->setData(color, ColorRole);
            colors_->appendRow(item);
        }
    }

    void showSelection(QColor color, bool resetCursor = false) {
        const QScopedValueRollback guard(projecting_, true);
        QModelIndex selected;
        for (int row = 0; row < colors_->rowCount(); ++row) {
            const auto index = colors_->index(row, 0);
            if (index.data(ColorRole).value<QColor>() == color) {
                selected = index;
                break;
            }
        }
        selectionModel()->select(selected, QItemSelectionModel::ClearAndSelect);
        if (resetCursor || !currentIndex().isValid()) {
            selectionModel()->setCurrentIndex(selected.isValid() ? selected : colors_->index(0, 0),
                                              QItemSelectionModel::NoUpdate);
        }
    }

protected:
    QItemSelectionModel::SelectionFlags selectionCommand(const QModelIndex &, const QEvent *) const override {
        // Arrow keys move focus without changing the confirmed color.
        return QItemSelectionModel::NoUpdate;
    }

    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return || event->key() == Qt::Key_Space) {
            activate(currentIndex());
            event->accept();
        } else {
            QListView::keyPressEvent(event);
        }
    }

private:
    void activate(const QModelIndex &index) {
        if (index.isValid() && isVisible() && isEnabled()) onSelect(index.data(ColorRole).value<QColor>());
    }

    QStandardItemModel *colors_;
    bool projecting_ = false;
};

class InkColorPopup final : public QWidget {
public:
    explicit InkColorPopup(QWidget *parent)
        : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint) {
        setObjectName(QStringLiteral("inkColorPalette"));
        setAccessibleName(InkColorPicker::tr("Color palette"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoMouseReplay);
        AppTheme::setTone(*this, AppTheme::Tone::Light);
        AppTheme::setStyleVariant(*this, "ink-color-popup");

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 10, 12, 10);
        layout->setSpacing(5);
        const auto presetText = InkColorPicker::tr("Preset colors");
        layout->addWidget(new QLabel(presetText, this));
        presets_ = new InkSwatchView(2, presetText, this);
        presets_->setObjectName(QStringLiteral("inkColorPresets"));
        presets_->setColors(presetColors(static_cast<int>(Presets.size())));
        layout->addWidget(presets_);

        const auto recentText = InkColorPicker::tr("Recent colors");
        recentLabel_ = new QLabel(recentText, this);
        layout->addWidget(recentLabel_);
        recent_ = new InkSwatchView(1, recentText, this);
        recent_->setObjectName(QStringLiteral("inkColorRecent"));
        layout->addWidget(recent_);

        layout->addWidget(new QLabel(InkColorPicker::tr("Custom color"), this));
        plane_ = new ColorPlane(this);
        layout->addWidget(plane_);
        hue_ = new QSlider(Qt::Horizontal, this);
        hue_->setObjectName(QStringLiteral("inkColorHue"));
        hue_->setAccessibleName(InkColorPicker::tr("Hue"));
        hue_->setRange(0, 359);
        layout->addWidget(hue_);

        auto *row = new QHBoxLayout;
        row->setSpacing(Gap);
        preview_ = new QLabel(this);
        preview_->setFixedSize(24, 24);
        row->addWidget(preview_);
        hex_ = new QLineEdit(this);
        hex_->setObjectName(QStringLiteral("inkColorHex"));
        hex_->setAccessibleName(InkColorPicker::tr("Hex color"));
        hex_->setPlaceholderText(QStringLiteral("#RRGGBB"));
        hex_->setMinimumWidth(85);
        row->addWidget(hex_, 1);
        apply_ = new QPushButton(InkColorPicker::tr("Apply"), this);
        apply_->setObjectName(QStringLiteral("inkColorApply"));
        row->addWidget(apply_);
        layout->addLayout(row);
        error_ = new QLabel(InkColorPicker::tr("Enter a color as #RRGGBB."), this);
        error_->setObjectName(QStringLiteral("inkColorError"));
        error_->setWordWrap(true);
        auto errorPolicy = error_->sizePolicy();
        errorPolicy.setRetainSizeWhenHidden(true);
        error_->setSizePolicy(errorPolicy);
        layout->addWidget(error_);

        presets_->onSelect = [this](QColor color) { onSelect(color); };
        recent_->onSelect = presets_->onSelect;
        plane_->onChange = [this](QColor color) { setPreview(color); };
        connect(hue_, &QSlider::valueChanged, plane_, &ColorPlane::setHue);
        connect(hex_, &QLineEdit::textChanged, this, [this](const QString &text) {
            static const QRegularExpression format(QStringLiteral("\\A#?[0-9a-fA-F]{6}\\z"));
            const auto value = text.trimmed();
            const bool valid = format.match(value).hasMatch();
            apply_->setEnabled(valid);
            error_->setVisible(!valid);
            if (!valid) return;
            const QColor color(value.startsWith(QLatin1Char('#')) ? value : QLatin1Char('#') + value);
            updatePreview(color);
            plane_->setColor(color);
            if (color.hsvHue() >= 0) {
                const QSignalBlocker blocker(hue_);
                hue_->setValue(color.hsvHue());
            }
        });
        const auto apply = [this] {
            if (apply_->isEnabled()) onSelect(candidate_);
        };
        connect(apply_, &QPushButton::clicked, this, apply);
        connect(hex_, &QLineEdit::returnPressed, this, apply);
        new QShortcut(QKeySequence(Qt::Key_Escape), this, [this] { hide(); },
                      Qt::WidgetWithChildrenShortcut);
        AppTheme::observeChanges(*this, [this] { update(); });
    }

    std::function<void(QColor)> onSelect;

    void openFor(QWidget &anchor, QColor current, const QList<QColor> &history) {
        presets_->showSelection(current, true);
        recent_->setColors(history);
        recentLabel_->setVisible(!history.isEmpty());
        recent_->setVisible(!history.isEmpty());
        recent_->showSelection(current, true);
        setPreview(current);
        plane_->setColor(current);
        {
            const QSignalBlocker blocker(hue_);
            hue_->setValue(plane_->hue());
        }
        layout()->activate();
        adjustSize();

        const auto top = anchor.mapToGlobal(QPoint(0, 0));
        const auto bottom = top + QPoint(0, anchor.height());
        auto *screen = QGuiApplication::screenAt(anchor.mapToGlobal(anchor.rect().center()));
        if (!screen) screen = anchor.screen();
        if (screen) {
            const auto area = screen->availableGeometry().adjusted(6, 6, -6, -6);
            int y = bottom.y() + Gap;
            if (y + height() > area.bottom() + 1) y = top.y() - height() - Gap;
            move(std::clamp(top.x(), area.left(), std::max(area.left(), area.right() - width() + 1)),
                 std::clamp(y, area.top(), std::max(area.top(), area.bottom() - height() + 1)));
        } else {
            move(bottom + QPoint(0, Gap));
        }
        show();
        presets_->setFocus(Qt::PopupFocusReason);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (rect().contains(event->pos())) {
            QWidget::mousePressEvent(event);
            return;
        }
        setAttribute(Qt::WA_NoMouseReplay);
        event->accept();
        hide();
    }

    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(AppTheme::color(AppTheme::ColorRole::Border));
        painter.setBrush(AppTheme::color(AppTheme::ColorRole::Surface));
        painter.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 9, 9);
    }

private:
    void setPreview(QColor color) {
        const QSignalBlocker blocker(hex_);
        hex_->setText(hex(color));
        error_->hide();
        apply_->setEnabled(true);
        updatePreview(color);
    }

    void updatePreview(QColor color) {
        candidate_ = color;
        const auto dpr = preview_->devicePixelRatioF();
        QPixmap swatch(QSize(24, 24) * dpr);
        swatch.setDevicePixelRatio(dpr);
        swatch.fill(Qt::transparent);
        QPainter painter(&swatch);
        drawSwatch(painter, QRectF(2, 2, 20, 20), color);
        painter.end();
        preview_->setPixmap(swatch);
        preview_->setAccessibleName(hex(color));
    }

    InkSwatchView *presets_;
    InkSwatchView *recent_;
    QLabel *recentLabel_;
    ColorPlane *plane_;
    QSlider *hue_;
    QLineEdit *hex_;
    QLabel *preview_;
    QLabel *error_;
    QPushButton *apply_;
    QColor candidate_;
};

InkColorPicker::InkColorPicker(const QString &buttonId, QColor initialColor, QWidget *parent)
    : QWidget(parent), current_(initialColor), button_(new QPushButton(tr("Color palette"), this)),
      shortcuts_(new InkSwatchView(1, tr("Quick colors"), this)), popup_(new InkColorPopup(this)) {
    button_->setObjectName(buttonId);
    button_->setCursor(Qt::PointingHandCursor);
    button_->setFocusPolicy(Qt::TabFocus);
    button_->setIconSize({18, 18});
    shortcuts_->setObjectName(QStringLiteral("inkColorShortcuts"));
    shortcuts_->setColors(presetColors(Columns));
    shortcuts_->showSelection(current_);
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(Gap);
    row->setSizeConstraint(QLayout::SetNoConstraint);
    row->addWidget(button_);
    row->addWidget(shortcuts_);
    row->addStretch();
    shortcuts_->onSelect = [this](QColor color) { selectColor(color); };
    popup_->onSelect = shortcuts_->onSelect;
    connect(button_, &QPushButton::clicked, this, &InkColorPicker::openPalette);
    updateButton();
}

QSize InkColorPicker::sizeHint() const {
    auto size = minimumSizeHint();
    size.rwidth() += Gap + shortcuts_->width();
    return size;
}

QSize InkColorPicker::minimumSizeHint() const { return button_->sizeHint(); }

void InkColorPicker::resizeEvent(QResizeEvent *event) {
    shortcuts_->setVisible(width() >= sizeHint().width());
    QWidget::resizeEvent(event);
}

void InkColorPicker::changeEvent(QEvent *event) {
    if (event->type() == QEvent::EnabledChange && !isEnabled()) closePalette();
    QWidget::changeEvent(event);
}

void InkColorPicker::hideEvent(QHideEvent *event) {
    closePalette();
    QWidget::hideEvent(event);
}

void InkColorPicker::openPalette() {
    if (isVisible() && isEnabled()) popup_->openFor(*button_, current_, recent_);
}

void InkColorPicker::closePalette() { popup_->hide(); }

void InkColorPicker::selectColor(QColor color) {
    if (!isEnabled() || !color.isValid()) return;
    closePalette();
    recent_.removeAll(color);
    recent_.prepend(color);
    if (recent_.size() > Columns) recent_.removeLast();
    if (color == current_) return;
    current_ = color;
    shortcuts_->showSelection(current_);
    updateButton();
    emit colorSelected(current_);
}

void InkColorPicker::updateButton() {
    const auto dpr = button_->devicePixelRatioF();
    QPixmap swatch(button_->iconSize() * dpr);
    swatch.setDevicePixelRatio(dpr);
    swatch.fill(Qt::transparent);
    QPainter painter(&swatch);
    drawSwatch(painter, QRectF(1, 1, 16, 16), current_);
    painter.end();
    button_->setIcon(QIcon(swatch));
    const auto label = tr("Ink color: %1").arg(colorName(current_));
    button_->setToolTip(label);
    button_->setAccessibleName(label);
    QAccessibleEvent event(button_, QAccessible::NameChanged);
    QAccessible::updateAccessibility(&event);
}
}
