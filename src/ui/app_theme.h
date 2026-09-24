#pragma once

#include <QtCore/QVariant>
#include <QtWidgets/QStyle>
#include <QtWidgets/QWidget>

class QApplication;
class QDialog;
class QMenu;
class QWidget;

namespace MeetingUI::AppTheme {

enum class Tone {
	Light,
	Dark,
};

void install(QApplication &application);
// State changes only select a rule from the application-wide startup sheet.
// Inline so small widget-only consumers use the same property contract.
inline void setStyleVariant(QWidget &widget, const char *variant) {
	if (widget.property("uiStyle").toByteArray() == variant) return;
	widget.setProperty("uiStyle", variant);
	const auto refresh = [](QWidget *target) {
		target->style()->unpolish(target);
		target->style()->polish(target);
		target->update();
	};
	refresh(&widget);
	for (auto *child : widget.findChildren<QWidget *>()) refresh(child);
}
void setTone(QWidget &widget, Tone tone);
void styleChoiceControls(QWidget &widget, Tone tone);
void styleMenu(QMenu &menu, Tone tone);
void centerOnScreen(QWidget &window);
// Use a real top-level window so modeless diagnostic surfaces keep the native
// movable title bar even when they have an owning meeting window.
void configureModelessWindow(QDialog &dialog);
// Grow to the content on opening; retain scrolling when it exceeds the screen.
void makeDialogAdaptive(QDialog &dialog, QSize preferredSize);

} // namespace MeetingUI::AppTheme
