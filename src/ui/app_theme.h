#pragma once

#include <QtCore/QVariant>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtWidgets/QStyle>
#include <QtWidgets/QWidget>
#include <functional>
#include <optional>

class QApplication;
class QDialog;
class QMenu;
class QWidget;

namespace MeetingUI::AppTheme {

enum class Tone {
	Light,
	Dark,
};

enum class Theme { System, Light, Dark, Teal };
enum class ColorRole {
	Window, Surface, SurfaceAlt, Hover, Pressed,
	Text, TextSecondary, TextMuted, Border,
	Accent, AccentHover, AccentPressed, AccentSoft, OnAccent,
	Success, Warning, Danger, Stage, Tile, Letterbox,
};

struct InstallOptions {
	// Optional platform probe for isolated hosts/tests. Only Light/Dark are valid.
	std::function<std::optional<Theme>()> systemThemeProbe;
};

// All theme access and notifications belong to the application/UI thread.
void install(QApplication &application, InstallOptions options = {});
Theme selectedTheme();
Theme effectiveTheme();
QString themeId(Theme theme);
Theme themeFromId(const QString &id);
QColor color(ColorRole role);
quint64 revision();
void setTheme(Theme theme);
void refreshSystemTheme();
void observeChanges(QObject &context, std::function<void()> callback);

// State changes select a rule from the current application-wide sheet.
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
// Shared palette for custom-painted panels as well as choice popups.
void applySurfacePalette(QWidget &widget, Tone tone);
void styleChoiceControls(QWidget &widget, Tone tone);
void styleMenu(QMenu &menu, Tone tone);
void centerOnScreen(QWidget &window);
// Use a real top-level window so modeless diagnostic surfaces keep the native
// movable title bar even when they have an owning meeting window.
void configureModelessWindow(QDialog &dialog);
// Grow to the content on opening; retain scrolling when it exceeds the screen.
void makeDialogAdaptive(QDialog &dialog, QSize preferredSize);

} // namespace MeetingUI::AppTheme
