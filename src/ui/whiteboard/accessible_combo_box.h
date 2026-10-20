#pragma once

class QComboBox;
class QWidget;

namespace MeetingUI {
// Qt 5 UIA SelectionItem must commit a choice, not just highlight the popup row.
QComboBox *createAccessibleComboBox(QWidget *parent);
// Bounded selectors keep full model/UIA text and reveal clipped text on hover.
QComboBox *createElidedAccessibleComboBox(QWidget *parent);
QComboBox *createAccessibleWhiteboardComboBox(QWidget *parent);
void configureAccessibleComboBox(QComboBox *combo);
}
