#pragma once

class QComboBox;
class QWidget;

namespace MeetingUI {
// Qt 5 UIA SelectionItem must commit a choice, not just highlight the popup row.
QComboBox *createAccessibleComboBox(QWidget *parent);
QComboBox *createAccessibleWhiteboardComboBox(QWidget *parent);
}
