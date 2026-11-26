#pragma once

#include <QtCore/QList>
#include <QtGui/QColor>
#include <QtWidgets/QWidget>

class QPushButton;

namespace MeetingUI {
class InkColorPopup;
class InkSwatchView;

class InkColorPicker final : public QWidget {
    Q_OBJECT
public:
    InkColorPicker(const QString &buttonId, QColor initialColor, QWidget *parent);
    void closePalette();
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void colorSelected(QColor color);

protected:
    void changeEvent(QEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void openPalette();
    void selectColor(QColor color);
    void updateButton();

    QColor current_;
    QList<QColor> recent_;
    QPushButton *button_;
    InkSwatchView *shortcuts_;
    InkColorPopup *popup_;
};
}
