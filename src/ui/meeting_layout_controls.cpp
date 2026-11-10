#include "src/ui/meeting_layout_controls.h"

#include "src/ui/app_theme.h"
#include "src/ui/app_icons.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QRegularExpression>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QHideEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtGui/QFontMetrics>
#include <QtGui/QRegularExpressionValidator>
#include <QtGui/QScreen>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QStyleOptionButton>
#include <QtWidgets/QStyleOptionToolButton>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>
#include <algorithm>

namespace MeetingUI {
namespace {

QString layoutText(const char *source) {
    return QCoreApplication::translate("MeetingRoomWindow", source);
}

void paintSurface(QWidget &widget) {
    QPainter painter(&widget);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(AppTheme::color(AppTheme::ColorRole::Border));
    painter.setBrush(AppTheme::color(AppTheme::ColorRole::Surface));
    painter.drawRoundedRect(QRectF(widget.rect()).adjusted(.5, .5, -.5, -.5), 9, 9);
}

// A separate popup HWND stays above the native video canvas. Owning it from
// its caller also lets Qt retain the parent popup when this nested popup closes.
class SurfacePopup final : public QWidget {
public:
    explicit SurfacePopup(QWidget *parent)
        : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoMouseReplay);
        AppTheme::setStyleVariant(*this, "meeting-layout-popup");
        AppTheme::observeChanges(*this, [this] { update(); });
    }

protected:
    void paintEvent(QPaintEvent *) override { paintSurface(*this); }
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape) {
            hide();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }
};

QRect availableFor(QWidget *anchor) {
    const auto point = anchor->mapToGlobal(anchor->rect().center());
    auto *screen = QGuiApplication::screenAt(point);
    if (!screen) screen = anchor->screen();
    return screen ? screen->availableGeometry() : QRect(point, QSize(800, 600));
}

void placePopup(QWidget &popup, QWidget *anchor, bool preferAbove = false) {
    const auto available = availableFor(anchor).adjusted(6, 6, -6, -6);
    popup.resize(popup.size().boundedTo(available.size()));
    const auto topLeft = anchor->mapToGlobal(QPoint(0, 0));
    const auto bottomRight = anchor->mapToGlobal(QPoint(anchor->width(), anchor->height()));
    auto x = bottomRight.x() - popup.width();
    auto y = preferAbove ? topLeft.y() - popup.height() - 6 : bottomRight.y() + 6;
    if (y + popup.height() > available.bottom() + 1) y = topLeft.y() - popup.height() - 6;
    if (y < available.top()) y = bottomRight.y() + 6;
    x = std::clamp(x, available.left(), std::max(available.left(), available.right() - popup.width() + 1));
    y = std::clamp(y, available.top(), std::max(available.top(), available.bottom() - popup.height() + 1));
    popup.move(x, y);
}

QToolButton *textButton(QWidget *parent, const QString &text, const char *name) {
    auto *button = new QToolButton(parent);
    button->setObjectName(QString::fromLatin1(name));
    button->setText(text);
    button->setAccessibleName(text);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::StrongFocus);
    return button;
}

class WrappingCheckBox final : public QCheckBox {
public:
    WrappingCheckBox(const QString &text, QWidget *parent) : QCheckBox(text, parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }
    QSize minimumSizeHint() const override { return QSize(180, 34); }
    QSize sizeHint() const override { return QSize(310, 34); }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override {
        const auto bounds = fontMetrics().boundingRect(QRect(0, 0, std::max(1, width - 24), 300),
            Qt::TextWordWrap, text());
        return std::max(24, bounds.height() + 4);
    }
protected:
    bool hitButton(const QPoint &position) const override { return rect().contains(position); }
    void paintEvent(QPaintEvent *) override {
        QStyleOptionButton option;
        initStyleOption(&option);
        option.text.clear();
        option.rect = QRect(0, (height() - 20) / 2, 20, 20);
        QPainter p(this);
        style()->drawControl(QStyle::CE_CheckBox, &option, &p, this);
        p.setPen(AppTheme::color(isEnabled() ? AppTheme::ColorRole::TextSecondary : AppTheme::ColorRole::TextMuted));
        p.drawText(QRect(24, 0, std::max(1, width() - 24), height()), Qt::TextWordWrap | Qt::AlignVCenter, text());
        if (hasFocus()) {
            p.setPen(QPen(AppTheme::color(AppTheme::ColorRole::Accent), 1, Qt::DotLine));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 3, 3);
        }
    }
};

constexpr std::array<livekit::SpeakerLayout, 5> kSpeakerLayouts = {
    livekit::SpeakerLayout::Right,
    livekit::SpeakerLayout::Top,
    livekit::SpeakerLayout::TopLeft,
    livekit::SpeakerLayout::BottomLeft,
    livekit::SpeakerLayout::Floating,
};

constexpr std::array<const char *, 5> kSpeakerLabels = {
    "Right member list", "Top member list", "Upper L", "Lower L", "Floating list",
};

// The thumbnails use the same theme tokens as the application. They are real
// buttons, so keyboard focus, Space/Enter activation and UIA work without an
// additional invisible hit target above the native canvas.
class LayoutCard final : public QAbstractButton {
public:
    LayoutCard(bool grid, livekit::SpeakerLayout layout, const QString &label,
               QWidget *parent)
        : QAbstractButton(parent), grid_(grid), layout_(layout) {
        setText(label);
        setAccessibleName(label);
        setCheckable(true);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setFixedSize(112, 108);
    }

    void setGridCapacity(uint32_t count) { capacity_ = count; update(); }

protected:
    void paintEvent(QPaintEvent *) override {
        using Role = AppTheme::ColorRole;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const bool highlighted = isChecked() || hasFocus();
        p.setPen(QPen(AppTheme::color(highlighted ? Role::Accent : Role::Border), highlighted ? 1.5 : 1));
        p.setBrush(AppTheme::color(isChecked() ? Role::AccentSoft : underMouse() ? Role::Hover : Role::Surface));
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 6, 6);
        const QRectF picture(9, 10, width() - 18, 60);
        auto small = AppTheme::color(Role::Accent);
        small.setAlpha(110);
        auto main = AppTheme::color(Role::Accent);
        main.setAlpha(150);
        const auto box = [&](QRectF r, bool isMain = false) {
            p.setPen(Qt::NoPen);
            p.setBrush(isMain ? main : small);
            p.drawRoundedRect(r, 1.5, 1.5);
        };
        if (grid_) {
            const int dimension = capacity_ == 49 ? 7 : capacity_ == 25 ? 5 : 3;
            const auto w = (picture.width() - (dimension - 1) * 2) / dimension;
            const auto h = (picture.height() - (dimension - 1) * 2) / dimension;
            for (int y = 0; y != dimension; ++y)
                for (int x = 0; x != dimension; ++x)
                    box(QRectF(picture.x() + x * (w + 2), picture.y() + y * (h + 2), w, h));
        } else if (layout_ == livekit::SpeakerLayout::Right) {
            box(QRectF(picture.x(), picture.y(), picture.width() * .72, picture.height()), true);
            for (int i = 0; i != 4; ++i)
                box(QRectF(picture.x() + picture.width() * .75, picture.y() + i * 15.5, picture.width() * .25, 13.5));
        } else if (layout_ == livekit::SpeakerLayout::Top) {
            box(QRectF(picture.x(), picture.y() + 18, picture.width(), picture.height() - 18), true);
            for (int i = 0; i != 4; ++i)
                box(QRectF(picture.x() + i * picture.width() / 4, picture.y(), picture.width() / 4 - 2, 15));
        } else if (layout_ == livekit::SpeakerLayout::Floating) {
            box(picture, true);
            const QRectF strip(picture.right() - 21, picture.y() + 5, 17, picture.height() - 10);
            p.setPen(QPen(AppTheme::color(Role::Border), 1));
            p.setBrush(AppTheme::color(Role::Surface));
            p.drawRoundedRect(strip.adjusted(-2, -2, 2, 2), 2, 2);
            for (int i = 0; i != 4; ++i)
                box(QRectF(strip.x(), strip.y() + i * 12.5, strip.width(), 10.5));
        } else {
            const bool upper = layout_ == livekit::SpeakerLayout::TopLeft;
            const auto stripY = upper ? picture.y() : picture.bottom() - 13;
            const auto mainY = upper ? picture.y() + 16 : picture.y();
            box(QRectF(picture.x(), mainY, picture.width() * .75 - 2, picture.height() - 16), true);
            for (int i = 0; i != 5; ++i)
                box(QRectF(picture.x() + i * picture.width() / 5, stripY, picture.width() / 5 - 2, 13));
            for (int i = 0; i != 3; ++i)
                box(QRectF(picture.x() + picture.width() * .75, mainY + i * 15, picture.width() * .25, 13));
        }
        p.setPen(AppTheme::color(isChecked() ? Role::Accent : Role::Text));
        p.drawText(QRect(4, 78, width() - 8, 24), Qt::AlignCenter, text());
    }
    void enterEvent(QEvent *event) override { QAbstractButton::enterEvent(event); update(); }
    void leaveEvent(QEvent *event) override { QAbstractButton::leaveEvent(event); update(); }
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            click();
            event->accept();
            return;
        }
        QAbstractButton::keyPressEvent(event);
    }

private:
    bool grid_ = false;
    livekit::SpeakerLayout layout_;
    uint32_t capacity_ = 9;
};

} // namespace

MeetingLayoutButton::MeetingLayoutButton(QWidget *parent)
    : QToolButton(parent), arrow_(AppTheme::icon(AppTheme::Icon::ChevronDown)) {
    setObjectName(QStringLiteral("meetingLayout"));
    setIconSize(QSize(16, 16));
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    AppTheme::setStyleVariant(*this, "meeting-layout-trigger");
    AppTheme::observeChanges(*this, [this] { requestGeometryUpdate(); });
    refreshCaption();
}

void MeetingLayoutButton::setSelection(livekit::VideoLayoutMode mode, livekit::SpeakerLayout speakerLayout) {
    mode = mode == livekit::VideoLayoutMode::Speaker || mode == livekit::VideoLayoutMode::PictureInPicture
        ? livekit::VideoLayoutMode::Speaker : livekit::VideoLayoutMode::Grid;
    if (mode_ == mode && speakerLayout_ == speakerLayout && !text().isEmpty()) return;
    mode_ = mode;
    speakerLayout_ = speakerLayout;
    refreshCaption();
}

void MeetingLayoutButton::refreshCaption() {
    using Icon = AppTheme::Icon;
    const char *caption = "Gallery";
    const char *description = "Gallery";
    auto glyph = Icon::LayoutGrid;
    if (mode_ == livekit::VideoLayoutMode::Speaker) {
        switch (speakerLayout_) {
        case livekit::SpeakerLayout::Right:
            caption = "Right layout"; description = "Right member list"; glyph = Icon::LayoutRight; break;
        case livekit::SpeakerLayout::Top:
            caption = "Top layout"; description = "Top member list"; glyph = Icon::LayoutTop; break;
        case livekit::SpeakerLayout::TopLeft:
            caption = "Upper L layout"; description = "Upper L"; glyph = Icon::LayoutUpperL; break;
        case livekit::SpeakerLayout::BottomLeft:
            caption = "Lower L layout"; description = "Lower L"; glyph = Icon::LayoutLowerL; break;
        case livekit::SpeakerLayout::Floating:
            caption = "Floating layout"; description = "Floating list"; glyph = Icon::LayoutFloating; break;
        }
    }
    setText(layoutText(caption));
    setIcon(AppTheme::icon(glyph));
    setAccessibleName(text());
    setAccessibleDescription(layoutText(description));
    setToolTip(layoutText(description));
    requestGeometryUpdate();
}

QSize MeetingLayoutButton::sizeHint() const {
    const_cast<MeetingLayoutButton *>(this)->ensurePolished();
    auto result = QToolButton::sizeHint();
    result.rwidth() += arrowColumnWidth();
    return result;
}

int MeetingLayoutButton::arrowColumnWidth() const {
    return std::max(12, style()->pixelMetric(QStyle::PM_MenuButtonIndicator, nullptr, this)) + 7;
}

void MeetingLayoutButton::requestGeometryUpdate() {
    updateGeometry();
    update();
    if (auto *parent = parentWidget()) QCoreApplication::postEvent(parent, new QEvent(QEvent::LayoutRequest));
}

bool MeetingLayoutButton::event(QEvent *event) {
    const auto type = event->type();
    const auto handled = QToolButton::event(event);
    if (type == QEvent::LanguageChange) refreshCaption();
    else if (type == QEvent::FontChange || type == QEvent::StyleChange ||
             type == QEvent::ContentsRectChange || type == QEvent::ScreenChangeInternal) requestGeometryUpdate();
    return handled;
}

void MeetingLayoutButton::paintEvent(QPaintEvent *) {
    QStyleOptionToolButton option;
    initStyleOption(&option);
    option.arrowType = Qt::NoArrow;
    option.features = QStyleOptionToolButton::None;
    QPainter painter(this);
    auto background = option;
    background.text.clear();
    background.icon = QIcon();
    style()->drawComplexControl(QStyle::CC_ToolButton, &background, &painter, this);
    const auto content = style()->subControlRect(QStyle::CC_ToolButton, &option, QStyle::SC_ToolButton, this);
    const auto columnWidth = arrowColumnWidth();
    option.rect = QStyle::visualRect(layoutDirection(), content, content.adjusted(0, 0, -columnWidth, 0));
    // Delegate font/icon spacing and label painting to the same style that
    // supplied QToolButton::sizeHint, including the active QSS padding/font.
    style()->drawControl(QStyle::CE_ToolButtonLabel, &option, &painter, this);
    const auto mode = isEnabled() ? QIcon::Normal : QIcon::Disabled;
    const QRect arrow(content.right() - columnWidth + 1 + (columnWidth - 12) / 2,
                      content.center().y() - 6, 12, 12);
    arrow_.paint(&painter, QStyle::visualRect(layoutDirection(), content, arrow), Qt::AlignCenter, mode);
}

void MeetingLayoutButton::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Down) {
        click();
        event->accept();
        return;
    }
    QToolButton::keyPressEvent(event);
}

MeetingLayoutPopup::MeetingLayoutPopup(QWidget *parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint) {
    setObjectName(QStringLiteral("meetingLayoutPopup"));
    setAccessibleName(layoutText("Layout"));
    setAttribute(Qt::WA_TranslucentBackground);
    // Closing on an outside click must not replay that click onto the layout
    // trigger and immediately open the same popup again.
    setAttribute(Qt::WA_NoMouseReplay);
    AppTheme::setStyleVariant(*this, "meeting-layout-popup");
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(10);
    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(layoutText("Layout"), this);
    auto font = title->font();
    font.setBold(true);
    title->setFont(font);
    heading->addWidget(title);
    heading->addStretch();
    auto *close = textButton(this, QString::fromUtf8("×"), "closeMeetingLayoutPopup");
    close->setAccessibleName(layoutText("Close"));
    close->setFixedSize(26, 26);
    connect(close, &QToolButton::clicked, this, &QWidget::hide);
    heading->addWidget(close);
    root->addLayout(heading);

    auto *collection = new QHBoxLayout;
    collection->setSpacing(10);
    auto *gridColumn = new QVBoxLayout;
    gridColumn->setSpacing(5);
    gridColumn->addWidget(new QLabel(layoutText("Gallery"), this));
    gridCard_ = new LayoutCard(true, livekit::SpeakerLayout::Right, layoutText("Gallery"), this);
    gridCard_->setObjectName(QStringLiteral("layoutGridCard"));
    gridColumn->addWidget(gridCard_);
    capacityButton_ = textButton(this, {}, "layoutGridCapacity");
    capacityButton_->setFixedHeight(26);
    capacityButton_->setAccessibleName(layoutText("Videos per page"));
    gridColumn->addWidget(capacityButton_);
    collection->addLayout(gridColumn);
    connect(gridCard_, &QAbstractButton::clicked, this, [this] {
        mode_ = livekit::VideoLayoutMode::Grid;
        refreshSelection();
        hide();
        if (layoutSelected) layoutSelected(mode_, speakerLayout_);
    });
    connect(capacityButton_, &QToolButton::clicked, this, &MeetingLayoutPopup::showCapacityPopup);

    auto *speakerColumn = new QVBoxLayout;
    speakerColumn->setSpacing(5);
    speakerColumn->addWidget(new QLabel(layoutText("Speaker layouts"), this));
    auto *ribbon = new QHBoxLayout;
    ribbon->setSpacing(5);
    previousCards_ = textButton(this, QString::fromUtf8("‹"), "previousLayoutCards");
    nextCards_ = textButton(this, QString::fromUtf8("›"), "nextLayoutCards");
    previousCards_->setAccessibleName(layoutText("Previous layouts"));
    nextCards_->setAccessibleName(layoutText("More layouts"));
    previousCards_->setFixedWidth(24);
    nextCards_->setFixedWidth(24);
    speakerScroll_ = new QScrollArea(this);
    speakerScroll_->setObjectName(QStringLiteral("speakerLayoutCards"));
    speakerScroll_->setFrameShape(QFrame::NoFrame);
    speakerScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    speakerScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    speakerScroll_->setWidgetResizable(false);
    speakerScroll_->setFixedHeight(108);
    speakerScroll_->setMinimumWidth(112);
    auto *cards = new QWidget(speakerScroll_);
    auto *cardsLayout = new QHBoxLayout(cards);
    cardsLayout->setContentsMargins(0, 0, 0, 0);
    cardsLayout->setSpacing(8);
    for (size_t i = 0; i != speakerCards_.size(); ++i) {
        auto *card = new LayoutCard(false, kSpeakerLayouts[i], layoutText(kSpeakerLabels[i]), cards);
        card->setObjectName(QStringLiteral("layoutSpeakerCard%1").arg(i));
        card->installEventFilter(this);
        cardsLayout->addWidget(card);
        speakerCards_[i] = card;
        connect(card, &QAbstractButton::clicked, this, [this, i] {
            mode_ = livekit::VideoLayoutMode::Speaker;
            speakerLayout_ = kSpeakerLayouts[i];
            refreshSelection();
            hide();
            if (layoutSelected) layoutSelected(mode_, speakerLayout_);
        });
    }
    cards->setFixedSize(5 * 112 + 4 * 8, 108);
    speakerScroll_->setWidget(cards);
    ribbon->addWidget(previousCards_);
    ribbon->addWidget(speakerScroll_, 1);
    ribbon->addWidget(nextCards_);
    speakerColumn->addLayout(ribbon);
    speakerColumn->addSpacing(26);
    collection->addLayout(speakerColumn, 1);
    root->addLayout(collection);
    speakingOnly_ = new WrappingCheckBox(layoutText("Show only speaking people in the member list"), this);
    speakingOnly_->setObjectName(QStringLiteral("layoutSpeakingOnly"));
    root->addWidget(speakingOnly_);
    connect(speakingOnly_, &QCheckBox::toggled, this, [this](bool checked) {
        if (speakingOnlyChanged) speakingOnlyChanged(checked);
    });
    connect(previousCards_, &QToolButton::clicked, this, [this] { scrollCards(-1); });
    connect(nextCards_, &QToolButton::clicked, this, [this] { scrollCards(1); });
    auto *scrollbar = speakerScroll_->horizontalScrollBar();
    const auto syncArrows = [this, scrollbar] {
        previousCards_->setEnabled(scrollbar->value() > scrollbar->minimum());
        nextCards_->setEnabled(scrollbar->value() < scrollbar->maximum());
    };
    connect(scrollbar, &QScrollBar::valueChanged, this, syncArrows);
    connect(scrollbar, &QScrollBar::rangeChanged, this, syncArrows);
    AppTheme::observeChanges(*this, [this] {
        update();
        gridCard_->update();
        for (auto *card : speakerCards_) card->update();
    });
    refreshSelection();
}

void MeetingLayoutPopup::setSelection(livekit::VideoLayoutMode mode,
                                      livekit::SpeakerLayout speakerLayout,
                                      uint32_t gridCapacity, bool speakingOnly) {
    mode_ = mode;
    speakerLayout_ = speakerLayout;
    gridCapacity_ = gridCapacity == 25 || gridCapacity == 49 ? gridCapacity : 9;
    const QSignalBlocker blocker(speakingOnly_);
    speakingOnly_->setChecked(speakingOnly);
    refreshSelection();
}

void MeetingLayoutPopup::refreshSelection() {
    gridCard_->setChecked(mode_ == livekit::VideoLayoutMode::Grid || mode_ == livekit::VideoLayoutMode::Auto);
    static_cast<LayoutCard *>(gridCard_)->setGridCapacity(gridCapacity_);
    capacityButton_->setText(layoutText("%1 per screen").arg(gridCapacity_) + QString::fromUtf8(" ▾"));
    for (size_t i = 0; i != speakerCards_.size(); ++i)
        speakerCards_[i]->setChecked(mode_ == livekit::VideoLayoutMode::Speaker && speakerLayout_ == kSpeakerLayouts[i]);
    constexpr std::array<uint32_t, 3> capacities{9, 25, 49};
    for (size_t i = 0; i != capacityOptions_.size(); ++i) {
        if (!capacityOptions_[i]) continue;
        capacityOptions_[i]->setChecked(gridCapacity_ == capacities[i]);
        capacityOptions_[i]->setText(layoutText("%1 per screen").arg(capacities[i])
            + (gridCapacity_ == capacities[i] ? QString::fromUtf8("  ✓") : QString()));
    }
}

void MeetingLayoutPopup::showAnchored(QWidget *anchor) {
    if (!anchor) return;
    if (isVisible()) { hide(); return; }
    const auto available = availableFor(anchor).adjusted(6, 6, -6, -6);
    const int ownerWidth = anchor->window() ? anchor->window()->width() : available.width();
    const int targetWidth = std::clamp(ownerWidth - 28, 330, 810);
    resize(std::min(targetWidth, available.width()), sizeHint().height());
    placePopup(*this, anchor);
    show();
    raise();
    revealSelection();
    // Ensure the scroll viewport has its final width before exposing the
    // accepted speaker card, including after a window or screen change.
    QTimer::singleShot(0, this, [this] { if (isVisible()) revealSelection(); });
}

void MeetingLayoutPopup::revealSelection() {
    for (auto *card : speakerCards_) {
        if (!card->isChecked()) continue;
        speakerScroll_->ensureWidgetVisible(card, 5, 0);
        card->setFocus(Qt::PopupFocusReason);
        return;
    }
    speakerScroll_->horizontalScrollBar()->setValue(0);
    gridCard_->setFocus(Qt::PopupFocusReason);
}

void MeetingLayoutPopup::showCapacityPopup() {
    if (!capacityPopup_) {
        capacityPopup_ = new SurfacePopup(this);
        capacityPopup_->setObjectName(QStringLiteral("layoutGridCapacityPopup"));
        capacityPopup_->setAccessibleName(layoutText("Videos per page"));
        auto *layout = new QVBoxLayout(capacityPopup_);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(3);
        layout->addWidget(new QLabel(layoutText("Videos per page"), capacityPopup_));
        constexpr std::array<uint32_t, 3> capacities{9, 25, 49};
        for (size_t i = 0; i != capacities.size(); ++i) {
            const auto count = capacities[i];
            auto *option = textButton(capacityPopup_, {}, "layoutGridCapacityOption");
            option->setObjectName(QStringLiteral("layoutGridCapacity%1").arg(count));
            option->setCheckable(true);
            option->setMinimumWidth(150);
            layout->addWidget(option);
            capacityOptions_[i] = option;
            connect(option, &QToolButton::clicked, this, [this, count] {
                gridCapacity_ = count;
                mode_ = livekit::VideoLayoutMode::Grid;
                refreshSelection();
                capacityPopup_->hide();
                capacityButton_->setFocus(Qt::PopupFocusReason);
                if (gridCapacitySelected) gridCapacitySelected(count);
            });
        }
    }
    if (capacityPopup_->isVisible()) { capacityPopup_->hide(); return; }
    refreshSelection();
    capacityPopup_->adjustSize();
    placePopup(*capacityPopup_, capacityButton_);
    capacityPopup_->show();
    capacityPopup_->raise();
    for (auto *option : capacityOptions_)
        if (option->isChecked()) option->setFocus(Qt::PopupFocusReason);
}

void MeetingLayoutPopup::scrollCards(int direction) {
    auto *scroll = speakerScroll_->horizontalScrollBar();
    const int step = std::max(120, speakerScroll_->viewport()->width() / 120 * 120);
    scroll->setValue(scroll->value() + direction * step);
}

bool MeetingLayoutPopup::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::FocusIn) {
        if (auto *card = qobject_cast<QWidget *>(watched))
            speakerScroll_->ensureWidgetVisible(card, 5, 0);
    }
    return QWidget::eventFilter(watched, event);
}

void MeetingLayoutPopup::paintEvent(QPaintEvent *) { paintSurface(*this); }

void MeetingLayoutPopup::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        if (capacityPopup_ && capacityPopup_->isVisible()) capacityPopup_->hide();
        else hide();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void MeetingLayoutPopup::hideEvent(QHideEvent *event) {
    if (capacityPopup_) capacityPopup_->hide();
    QWidget::hideEvent(event);
}

MeetingVideoPager::MeetingVideoPager(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("videoPagingControls"));
    setAccessibleName(layoutText("Video pages"));
    AppTheme::setStyleVariant(*this, "meeting-video-pager");
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->setSpacing(4);
    layout->addStretch();
    previous_ = textButton(this, QString::fromUtf8("‹"), "previousVideoPage");
    previous_->setAccessibleName(layoutText("Previous video page"));
    previous_->setToolTip(previous_->accessibleName());
    previous_->setFixedSize(28, 28);
    indicator_ = textButton(this, QStringLiteral("1 / 1"), "videoPageIndicator");
    indicator_->setMinimumSize(64, 28);
    indicator_->setToolTip(layoutText("Jump to page"));
    next_ = textButton(this, QString::fromUtf8("›"), "nextVideoPage");
    next_->setAccessibleName(layoutText("Next video page"));
    next_->setToolTip(next_->accessibleName());
    next_->setFixedSize(28, 28);
    layout->addWidget(previous_);
    layout->addWidget(indicator_);
    layout->addWidget(next_);
    layout->addStretch();
    connect(previous_, &QToolButton::clicked, this, [this] {
        if (page_ && pageRequested) pageRequested(page_ - 1);
    });
    connect(next_, &QToolButton::clicked, this, [this] {
        if (page_ < count_ - 1 && pageRequested) pageRequested(page_ + 1);
    });
    connect(indicator_, &QToolButton::clicked, this, &MeetingVideoPager::showJumpPopup);
    setPage(0, 1);
}

void MeetingVideoPager::setPage(uint32_t page, uint32_t count) {
    count_ = std::max(1u, count);
    page_ = std::min(page, count_ - 1);
    previous_->setEnabled(page_ > 0);
    next_->setEnabled(page_ < count_ - 1);
    indicator_->setText(QStringLiteral("%1 / %2").arg(page_ + 1).arg(count_));
    indicator_->setAccessibleName(layoutText("Page %1 of %2. Jump to page.").arg(page_ + 1).arg(count_));
    indicator_->setEnabled(count_ > 1);
    // Membership/layout updates refresh only the range, never the draft.
    if (jumpRange_) jumpRange_->setText(layoutText("Page 1 to %1").arg(count_));
    if (count_ == 1 && jumpPopup_) jumpPopup_->hide();
}

void MeetingVideoPager::showJumpPopup() {
    if (count_ <= 1) return;
    if (!jumpPopup_) {
        jumpPopup_ = new SurfacePopup(this);
        jumpPopup_->setObjectName(QStringLiteral("videoPageJumpPopup"));
        jumpPopup_->setAccessibleName(layoutText("Jump to page"));
        auto *root = new QVBoxLayout(jumpPopup_);
        root->setContentsMargins(14, 12, 14, 12);
        root->setSpacing(8);
        root->addWidget(new QLabel(layoutText("Jump to page"), jumpPopup_));
        auto *row = new QHBoxLayout;
        jumpInput_ = new QLineEdit(jumpPopup_);
        jumpInput_->setObjectName(QStringLiteral("videoPageJumpInput"));
        jumpInput_->setAccessibleName(layoutText("Page number"));
        jumpInput_->setMaximumWidth(92);
        jumpInput_->setMaxLength(10);
        jumpInput_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9]{0,10}")), jumpInput_));
        auto *submit = textButton(jumpPopup_, layoutText("Go"), "videoPageJumpSubmit");
        row->addWidget(jumpInput_);
        row->addWidget(submit);
        root->addLayout(row);
        jumpRange_ = new QLabel(jumpPopup_);
        jumpRange_->setObjectName(QStringLiteral("videoPageJumpRange"));
        root->addWidget(jumpRange_);
        jumpError_ = new QLabel(jumpPopup_);
        jumpError_->setObjectName(QStringLiteral("videoPageJumpError"));
        jumpError_->setWordWrap(true);
        jumpError_->setMaximumWidth(230);
        jumpError_->hide();
        root->addWidget(jumpError_);
        connect(jumpInput_, &QLineEdit::returnPressed, this, &MeetingVideoPager::submitJump);
        connect(submit, &QToolButton::clicked, this, &MeetingVideoPager::submitJump);
        connect(jumpInput_, &QLineEdit::textEdited, this, [this] { jumpError_->hide(); });
    }
    if (jumpPopup_->isVisible()) { jumpPopup_->hide(); return; }
    jumpInput_->setText(QString::number(page_ + 1));
    jumpRange_->setText(layoutText("Page 1 to %1").arg(count_));
    jumpError_->hide();
    jumpPopup_->adjustSize();
    placePopup(*jumpPopup_, indicator_, true);
    jumpPopup_->show();
    jumpPopup_->raise();
    jumpInput_->setFocus(Qt::PopupFocusReason);
    jumpInput_->selectAll();
}

void MeetingVideoPager::submitJump() {
    if (!jumpInput_ || count_ <= 1) return;
    bool valid = false;
    const auto requested = jumpInput_->text().toULongLong(&valid);
    if (!valid || !requested || requested > count_) {
        jumpError_->setText(layoutText("Enter a page from 1 to %1.").arg(count_));
        jumpError_->show();
        jumpPopup_->adjustSize();
        placePopup(*jumpPopup_, indicator_, true);
        jumpInput_->setFocus();
        return;
    }
    jumpPopup_->hide();
    indicator_->setFocus(Qt::PopupFocusReason);
    if (pageRequested) pageRequested(static_cast<uint32_t>(requested - 1));
}

void MeetingVideoPager::hideEvent(QHideEvent *event) {
    if (jumpPopup_) jumpPopup_->hide();
    QWidget::hideEvent(event);
}

} // namespace MeetingUI
