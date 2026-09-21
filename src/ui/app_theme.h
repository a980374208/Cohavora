#pragma once

class QApplication;
class QMenu;
class QWidget;

namespace MeetingUI::AppTheme {

enum class Tone {
	Light,
	Dark,
};

void install(QApplication &application);
void setTone(QWidget &widget, Tone tone);
void styleChoiceControls(QWidget &widget, Tone tone);
void styleMenu(QMenu &menu, Tone tone);
void centerOnScreen(QWidget &window);

} // namespace MeetingUI::AppTheme
