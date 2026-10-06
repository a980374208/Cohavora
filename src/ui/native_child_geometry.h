#pragma once

#include <QtCore/QEvent>
#include <QtCore/QPointer>
#include <QtGui/QWindow>
#include <QtWidgets/QWidget>
#include <vector>

namespace MeetingUI {

// Qt does not move a native child when only an alien ancestor moves. Keep the
// native surface aligned without promoting the surrounding raster widgets.
class NativeChildGeometry final : public QObject {
public:
    explicit NativeChildGeometry(QWidget &widget) : QObject(&widget), widget_(widget) {
        widget_.setAttribute(Qt::WA_DontCreateNativeAncestors);
        widget_.installEventFilter(this);
        observeAncestors();
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::ParentChange) {
            observeAncestors();
        }
        switch (event->type()) {
        case QEvent::Show:
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::WinIdChange:
        case QEvent::ParentChange:
            sync();
            break;
        default:
            break;
        }
        return false;
    }

private:
    void observeAncestors() {
        for (const auto &ancestor : ancestors_) {
            if (ancestor) ancestor->removeEventFilter(this);
        }
        ancestors_.clear();
        for (auto *ancestor = widget_.parentWidget(); ancestor && !ancestor->isWindow();
             ancestor = ancestor->parentWidget()) {
            ancestor->installEventFilter(this);
            ancestors_.emplace_back(ancestor);
        }
    }

    void sync() {
        auto *handle = widget_.windowHandle();
        auto *parent = widget_.nativeParentWidget();
        if (!widget_.internalWinId() || !handle || !parent || widget_.isWindow()) return;
        const QRect geometry(widget_.mapTo(parent, QPoint()), widget_.size());
        if (handle->geometry() != geometry) handle->setGeometry(geometry);
    }

    QWidget &widget_;
    std::vector<QPointer<QWidget>> ancestors_;
};

} // namespace MeetingUI
