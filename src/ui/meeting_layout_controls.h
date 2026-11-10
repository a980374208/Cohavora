#pragma once

#include "src/core/video_demand_types.h"

#include <QtCore/QPointer>
#include <QtWidgets/QWidget>
#include <QtWidgets/QToolButton>
#include <array>
#include <cstdint>
#include <functional>

class QAbstractButton;
class QCheckBox;
class QHideEvent;
class QLabel;
class QLineEdit;
class QScrollArea;
class QToolButton;

namespace MeetingUI {

// The label remains real QToolButton text for keyboard/UIA. The styled button
// supplies its content size; the custom chevron adds one reserved column.
class MeetingLayoutButton final : public QToolButton {
public:
    explicit MeetingLayoutButton(QWidget *parent = nullptr);
    void setSelection(livekit::VideoLayoutMode mode, livekit::SpeakerLayout speakerLayout);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void refreshCaption();
    void requestGeometryUpdate();
    int arrowColumnWidth() const;
    livekit::VideoLayoutMode mode_ = livekit::VideoLayoutMode::Grid;
    livekit::SpeakerLayout speakerLayout_ = livekit::SpeakerLayout::Right;
    QIcon arrow_;
};

// Both surfaces are UI-owned and emit requests only. The accepted policy plan
// remains the source of truth for the stage and the page indicator.
class MeetingLayoutPopup final : public QWidget {
public:
    explicit MeetingLayoutPopup(QWidget *parent = nullptr);

    void setSelection(livekit::VideoLayoutMode mode,
                      livekit::SpeakerLayout speakerLayout,
                      uint32_t gridCapacity,
                      bool speakingOnly);
    void showAnchored(QWidget *anchor);

    std::function<void(livekit::VideoLayoutMode, livekit::SpeakerLayout)> layoutSelected;
    std::function<void(uint32_t)> gridCapacitySelected;
    std::function<void(bool)> speakingOnlyChanged;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void refreshSelection();
    void showCapacityPopup();
    void scrollCards(int direction);
    void revealSelection();

    livekit::VideoLayoutMode mode_ = livekit::VideoLayoutMode::Grid;
    livekit::SpeakerLayout speakerLayout_ = livekit::SpeakerLayout::Right;
    uint32_t gridCapacity_ = 9;
    QAbstractButton *gridCard_ = nullptr;
    QToolButton *capacityButton_ = nullptr;
    QCheckBox *speakingOnly_ = nullptr;
    QScrollArea *speakerScroll_ = nullptr;
    QToolButton *previousCards_ = nullptr;
    QToolButton *nextCards_ = nullptr;
    std::array<QAbstractButton *, 5> speakerCards_{};
    QWidget *capacityPopup_ = nullptr;
    std::array<QToolButton *, 3> capacityOptions_{};
};

class MeetingVideoPager final : public QWidget {
public:
    explicit MeetingVideoPager(QWidget *parent = nullptr);
    void setPage(uint32_t page, uint32_t count);
    std::function<void(uint32_t)> pageRequested;

protected:
    void hideEvent(QHideEvent *event) override;

private:
    void showJumpPopup();
    void submitJump();

    uint32_t page_ = 0;
    uint32_t count_ = 1;
    QToolButton *previous_ = nullptr;
    QToolButton *indicator_ = nullptr;
    QToolButton *next_ = nullptr;
    QWidget *jumpPopup_ = nullptr;
    QLineEdit *jumpInput_ = nullptr;
    QLabel *jumpRange_ = nullptr;
    QLabel *jumpError_ = nullptr;
};

} // namespace MeetingUI
