#pragma once

#include "src/telemetry/ui_timing_measurement.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtWidgets/QWidget>
#include <functional>

namespace MeetingUI {

// A bounded observation, not a business timeout. Owns only a value sample and
// guarded UI pointers, and survives deletion of the meeting window on leave.
inline void ObserveTimingBoundary(
        std::shared_ptr<livekit::diagnostic::UiTimingMeasurement> sample,
        std::function<bool()> visibleBoundary,
        std::chrono::milliseconds budget = std::chrono::seconds(120)) {
    auto *timer = new QTimer(QCoreApplication::instance());
    const auto deadline = livekit::diagnostic::UiTimingMeasurement::Clock::now() + budget;
    QObject::connect(timer, &QTimer::timeout, timer, [timer, sample, visibleBoundary, deadline] {
        using namespace livekit::diagnostic;
        if (!sample->terminal() && sample->businessReady && visibleBoundary())
            sample->Finish(sample->businessOutcome);
        if (!sample->terminal() && UiTimingMeasurement::Clock::now() >= deadline)
            sample->Finish(Outcome::Timeout);
        if (sample->terminal()) { timer->stop(); timer->deleteLater(); }
    });
    timer->start(10);
}

// Observes real page content, never the translucent window/background alone.
// The queued callback is after the paint dispatch, not a display-present fence.
class FirstPaintTimingObserver final : public QObject {
public:
    FirstPaintTimingObserver(QWidget *content,
            std::shared_ptr<livekit::diagnostic::UiTimingMeasurement> sample)
        : QObject(content), content_(content), sample_(std::move(sample)) {
        content->installEventFilter(this);
    }
    ~FirstPaintTimingObserver() override { sample_->Finish(livekit::diagnostic::Outcome::Cancelled); }
protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::Paint && !scheduled_ &&
            !sample_->terminal() && content_ && content_->isVisible() &&
            !content_->size().isEmpty()) {
            scheduled_ = true;
            QTimer::singleShot(0, this, [this] {
                scheduled_ = false;
                if (content_ && content_->isVisible() && !content_->size().isEmpty())
                    sample_->Finish(livekit::diagnostic::Outcome::Success);
            });
        }
        return QObject::eventFilter(watched, event);
    }
private:
    QPointer<QWidget> content_;
    std::shared_ptr<livekit::diagnostic::UiTimingMeasurement> sample_;
    bool scheduled_ = false;
};

} // namespace MeetingUI
