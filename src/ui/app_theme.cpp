#include "src/ui/app_theme.h"

#include <QtCore/QEvent>
#include <QtCore/QAbstractNativeEventFilter>
#include <QtCore/QFile>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QScopedValueRollback>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QVariant>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QPalette>
#include <QtGui/QScreen>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QStyle>
#include <QtWidgets/QWidget>
#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>

#if defined(Q_OS_WIN)
#include <dwmapi.h>
#include <windows.h>
#endif

namespace MeetingUI::AppTheme {
namespace {

constexpr auto kToneProperty = "meetingUiTone";
constexpr auto kRoleProperty = "meetingUiRole";
constexpr auto kInstalledProperty = "meetingUiThemeInstalled";
constexpr auto kMenuStyledToneProperty = "meetingUiMenuStyledTone";
constexpr auto kDialogStyledToneProperty = "meetingUiDialogStyledTone";
constexpr auto kCenteredProperty = "meetingUiCentered";
constexpr auto kSurfacePaletteProperty = "meetingUiSurfacePalette";
constexpr auto kThemeSetting = "ui/theme";

class ThemeRuntime;
QPointer<ThemeRuntime> runtime;

Tone currentTone() { return effectiveTheme() == Theme::Dark ? Tone::Dark : Tone::Light; }
bool installed() { return qApp && qApp->property(kInstalledProperty).toBool(); }
Tone resolvedTone(Tone tone) { return installed() ? currentTone() : tone; }

using Colors = std::array<const char *, 20>;
const Colors &themeColors(Theme theme) {
	static constexpr Colors light = {
		"#f5f7fa", "#ffffff", "#f0f2f5", "#e8edf4", "#dce3ed",
		"#1f2329", "#4e5969", "#667080", "#dfe3e8",
		"#0969da", "#0759b8", "#064b9d", "#eaf3ff", "#ffffff",
		"#15803d", "#b45309", "#d9363e", "#e9edf3", "#f8fafc", "#e9edf3",
	};
	static constexpr Colors dark = {
		"#171a20", "#242831", "#2b303a", "#353c49", "#404959",
		"#f3f4f6", "#bcc4d1", "#939dab", "#454f60",
		"#65b5ff", "#8ac8ff", "#3c9aee", "#213f5e", "#102338",
		"#65d494", "#f4c26a", "#ff8b94", "#121820", "#242d39", "#18212c",
	};
	static constexpr Colors teal = {
		"#eaf4f0", "#fbfffd", "#dfede7", "#d0e5dc", "#c1dcd0",
		"#183c32", "#42685b", "#586f65", "#bbd2c7",
		"#087f65", "#076e58", "#055b49", "#d9f1e7", "#ffffff",
		"#157541", "#a35d09", "#c4374d", "#d6e9e0", "#f2faf6", "#d6e9e0",
	};
	return theme == Theme::Dark ? dark : theme == Theme::Teal ? teal : light;
}

QPalette applicationPalette() {
	QPalette palette;
	const auto assign = [&](QPalette::ColorRole target, ColorRole source) {
		palette.setColor(QPalette::All, target, color(source));
	};
	assign(QPalette::Window, ColorRole::Window);
	assign(QPalette::WindowText, ColorRole::Text);
	assign(QPalette::Base, ColorRole::Surface);
	assign(QPalette::AlternateBase, ColorRole::SurfaceAlt);
	assign(QPalette::Text, ColorRole::Text);
	assign(QPalette::Button, ColorRole::SurfaceAlt);
	assign(QPalette::ButtonText, ColorRole::Text);
	assign(QPalette::Highlight, ColorRole::AccentSoft);
	assign(QPalette::HighlightedText, ColorRole::Accent);
	assign(QPalette::ToolTipBase, ColorRole::Surface);
	assign(QPalette::ToolTipText, ColorRole::Text);
	assign(QPalette::Link, ColorRole::Accent);
	assign(QPalette::LinkVisited, ColorRole::Success);
	assign(QPalette::Mid, ColorRole::Border);
	assign(QPalette::Dark, ColorRole::Border);
	assign(QPalette::Light, ColorRole::Surface);
	assign(QPalette::BrightText, ColorRole::Danger);
	for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText,
		QPalette::HighlightedText, QPalette::PlaceholderText}) {
		palette.setColor(QPalette::Disabled, role, color(ColorRole::TextMuted));
	}
	palette.setColor(QPalette::Active, QPalette::PlaceholderText, color(ColorRole::TextMuted));
	palette.setColor(QPalette::Inactive, QPalette::PlaceholderText, color(ColorRole::TextMuted));
	return palette;
}

QString renderSheet(QString sheet) {
	static constexpr std::array<const char *, 20> names = {
		"window", "surface", "surface-alt", "hover", "pressed",
		"text", "text-secondary", "text-muted", "border",
		"accent", "accent-hover", "accent-pressed", "accent-soft", "on-accent",
		"success", "warning", "danger", "stage", "tile", "letterbox",
	};
	for (size_t i = 0; i != names.size(); ++i) {
		sheet.replace(QStringLiteral("@%1@").arg(QString::fromLatin1(names[i])),
			color(static_cast<ColorRole>(i)).name());
	}
	for (const auto &entry : {std::pair{"success", ColorRole::Success},
		std::pair{"warning", ColorRole::Warning}, std::pair{"danger", ColorRole::Danger}}) {
		const auto foreground = color(entry.second);
		const auto background = color(ColorRole::Surface);
		const QColor soft((foreground.red()*12 + background.red()*88)/100,
			(foreground.green()*12 + background.green()*88)/100,
			(foreground.blue()*12 + background.blue()*88)/100);
		sheet.replace(QStringLiteral("@%1-soft@").arg(QString::fromLatin1(entry.first)), soft.name());
		sheet.replace(QStringLiteral("@%1-hover@").arg(QString::fromLatin1(entry.first)),
			effectiveTheme() == Theme::Dark ? foreground.lighter(112).name() : foreground.darker(110).name());
		sheet.replace(QStringLiteral("@%1-pressed@").arg(QString::fromLatin1(entry.first)),
			foreground.darker(115).name());
	}
	const auto rgba = [](const QColor &value) {
		return QStringLiteral("%1, %2, %3").arg(value.red()).arg(value.green()).arg(value.blue());
	};
	sheet.replace(QStringLiteral("@text-rgb@"), rgba(color(ColorRole::Text)));
	sheet.replace(QStringLiteral("@surface-rgb@"), rgba(color(ColorRole::Surface)));
	sheet.replace(QStringLiteral("@accent-rgb@"), rgba(color(ColorRole::Accent)));
	sheet.replace(QStringLiteral("@success-rgb@"), rgba(color(ColorRole::Success)));
	sheet.replace(QStringLiteral("@warning-rgb@"), rgba(color(ColorRole::Warning)));
	sheet.replace(QStringLiteral("@danger-rgb@"), rgba(color(ColorRole::Danger)));
	sheet.replace(QStringLiteral("@arrow-tone@"), effectiveTheme() == Theme::Dark
		? QStringLiteral("dark") : QStringLiteral("light"));
	return sheet;
}

std::optional<Theme> platformTheme() {
#if defined(Q_OS_WIN)
	QSettings settings(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
		QSettings::NativeFormat);
	const auto value = settings.value(QStringLiteral("AppsUseLightTheme"));
	bool valid = false;
	const auto mode = value.toInt(&valid);
	if (valid && settings.status() == QSettings::NoError && (mode == 0 || mode == 1)) {
		return mode == 0 ? Theme::Dark : Theme::Light;
	}
#endif
	return std::nullopt;
}

const char *toneName(Tone tone) {
	return tone == Tone::Dark ? "dark" : "light";
}

Tone toneFromWidget(const QWidget *widget) {
	if (installed()) return currentTone();
	for (auto *current = widget; current; current = current->parentWidget()) {
		const auto value = current->property(kToneProperty).toByteArray();
		if (value == "dark") {
			return Tone::Dark;
		}
		if (value == "light") {
			return Tone::Light;
		}
	}
	return Tone::Light;
}

void refreshStyle(QWidget &widget) {
	if (auto *style = widget.style()) {
		style->unpolish(&widget);
		style->polish(&widget);
	}
	widget.update();
}

QScreen *screenForWindow(const QWidget &window) {
	const QWidget *anchor = window.parentWidget();
	while (anchor && !anchor->isWindow()) {
		anchor = anchor->parentWidget();
	}
	if (!anchor) {
		anchor = &window;
	}
	if (auto *screen = QGuiApplication::screenAt(anchor->frameGeometry().center())) {
		return screen;
	}
	if (auto *screen = QGuiApplication::screenAt(QCursor::pos())) {
		return screen;
	}
	return QGuiApplication::primaryScreen();
}

void setButtonRole(QAbstractButton *button, const char *role) {
	if (!button) {
		return;
	}
	button->setProperty(kRoleProperty, role);
	refreshStyle(*button);
}

const char *messageButtonRole(QMessageBox::ButtonRole role) {
	switch (role) {
	case QMessageBox::AcceptRole:
	case QMessageBox::YesRole:
	case QMessageBox::ApplyRole:
		return "primary";
	case QMessageBox::DestructiveRole:
		return "danger";
	case QMessageBox::HelpRole:
		return "link";
	default:
		return "secondary";
	}
}

const char *dialogButtonRole(QDialogButtonBox::ButtonRole role) {
	switch (role) {
	case QDialogButtonBox::AcceptRole:
	case QDialogButtonBox::YesRole:
	case QDialogButtonBox::ApplyRole:
		return "primary";
	case QDialogButtonBox::DestructiveRole:
		return "danger";
	case QDialogButtonBox::HelpRole:
		return "link";
	default:
		return "secondary";
	}
}

void styleDialogButtons(QDialogButtonBox &buttonBox) {
	for (auto *button : buttonBox.buttons()) {
		setButtonRole(button, dialogButtonRole(buttonBox.buttonRole(button)));
	}
}

void configureMessageBox(QMessageBox &box, Tone tone) {
	box.setProperty(kToneProperty, toneName(tone));
	box.setMinimumWidth(460);
	if (auto *label = box.findChild<QLabel *>(QStringLiteral("qt_msgbox_label"))) {
		label->setWordWrap(true);
		label->setTextFormat(Qt::PlainText);
		label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
		label->setMinimumWidth(340);
		label->setMaximumWidth(520);
	}
	for (auto *button : box.buttons()) {
		setButtonRole(button, messageButtonRole(box.buttonRole(button)));
	}
	refreshStyle(box);
}

void configureInputDialog(QInputDialog &dialog, Tone tone) {
	dialog.setProperty(kToneProperty, toneName(tone));
	dialog.setMinimumWidth(420);
	for (auto *buttonBox : dialog.findChildren<QDialogButtonBox *>()) {
		styleDialogButtons(*buttonBox);
	}
	refreshStyle(dialog);
}

void configureProgressDialog(QProgressDialog &dialog, Tone tone) {
	dialog.setProperty(kToneProperty, toneName(tone));
	dialog.setMinimumWidth(380);
	for (auto *button : dialog.findChildren<QPushButton *>()) {
		setButtonRole(button, "secondary");
	}
	refreshStyle(dialog);
}

void applyNativeCorners(QWidget &widget, Tone tone) {
#if defined(Q_OS_WIN)
	if (widget.testAttribute(Qt::WA_TranslucentBackground)) {
		return;
	}
	auto handle = reinterpret_cast<HWND>(widget.winId());
	if (!handle) {
		return;
	}
	DWORD preference = 2; // DWMWCP_ROUND
	DwmSetWindowAttribute(
		handle,
		33 /* DWMWA_WINDOW_CORNER_PREFERENCE */,
		&preference,
		sizeof(preference));
	BOOL darkTitleBar = tone == Tone::Dark ? TRUE : FALSE;
	DwmSetWindowAttribute(
		handle,
		20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
		&darkTitleBar,
		sizeof(darkTitleBar));
#else
	Q_UNUSED(widget);
	Q_UNUSED(tone);
#endif
}

void centerOnParentOrScreen(QWidget &window) {
	auto *parentWindow = window.parentWidget();
	while (parentWindow && !parentWindow->isWindow()) {
		parentWindow = parentWindow->parentWidget();
	}
	if (!parentWindow || !parentWindow->isVisible()) {
		centerOnScreen(window);
		return;
	}

	auto size = window.frameGeometry().size();
	if (size.isEmpty()) {
		size = window.size();
	}
	const auto parentGeometry = parentWindow->frameGeometry();
	auto *screen = QGuiApplication::screenAt(parentGeometry.center());
	if (!screen) {
		screen = screenForWindow(window);
	}
	if (!screen) {
		return;
	}

	const auto available = screen->availableGeometry();
	const auto desiredX = parentGeometry.center().x() - size.width() / 2;
	const auto desiredY = parentGeometry.center().y() - size.height() / 2;
	const auto maxX = qMax(available.left(), available.right() - size.width() + 1);
	const auto maxY = qMax(available.top(), available.bottom() - size.height() + 1);
	window.move(
		qBound(available.left(), desiredX, maxX),
		qBound(available.top(), desiredY, maxY));
}

class ThemeEventFilter final : public QObject {
public:
	using QObject::QObject;

protected:
	bool eventFilter(QObject *watched, QEvent *event) override {
		if (event->type() == QEvent::Polish && !stylingChoice_) {
			if (auto *choice = qobject_cast<QComboBox *>(watched)) {
				QScopedValueRollback<bool> guard(stylingChoice_, true);
				styleChoiceControls(*choice, toneFromWidget(choice));
			}
		}
		if (auto *menu = qobject_cast<QMenu *>(watched)) {
			if (event->type() != QEvent::Polish && event->type() != QEvent::Show) {
				return QObject::eventFilter(watched, event);
			}
			const auto tone = toneFromWidget(menu);
			if (menu->property(kMenuStyledToneProperty).toByteArray() != toneName(tone)) {
				styleMenu(*menu, tone);
			}
			if (event->type() == QEvent::Show) {
				applyNativeCorners(*menu, tone);
			}
			return QObject::eventFilter(watched, event);
		}
		if (event->type() != QEvent::Polish && event->type() != QEvent::Show) {
			return QObject::eventFilter(watched, event);
		}

		auto *dialog = qobject_cast<QDialog *>(watched);
		if (!dialog) {
			return QObject::eventFilter(watched, event);
		}

		auto *box = qobject_cast<QMessageBox *>(dialog);
		auto *input = qobject_cast<QInputDialog *>(dialog);
		auto *progress = qobject_cast<QProgressDialog *>(dialog);
		const auto tone = toneFromWidget(dialog);
		if (box || input || progress) {
			const auto styledTone = dialog->property(kDialogStyledToneProperty).toByteArray();
			if (styledTone != toneName(tone)) {
				dialog->setProperty(kDialogStyledToneProperty, toneName(tone));
				if (box) {
					configureMessageBox(*box, tone);
				} else if (input) {
					configureInputDialog(*input, tone);
				} else {
					configureProgressDialog(*progress, tone);
				}
			}
			if (box && event->type() == QEvent::Show) {
				// QMessageBox::showEvent recalculates and can replace a minimum
				// requested by the caller. Restore it after Qt finishes that layout.
				const auto requestedMinimum = box->minimumSize().expandedTo(QSize(460, 0));
				box->adjustSize();
				QPointer<QMessageBox> guard(box);
				QTimer::singleShot(0, box, [guard, requestedMinimum] {
					if (!guard) return;
					guard->setMinimumSize(requestedMinimum);
					guard->resize(guard->size().expandedTo(requestedMinimum));
					centerOnParentOrScreen(*guard);
				});
			}
		} else if (event->type() == QEvent::Show
			&& !dialog->property(kToneProperty).isValid()) {
			dialog->setProperty(kToneProperty, toneName(tone));
		}
		if (event->type() == QEvent::Show) {
			if (dialog->property("meetingUiAdaptive").toBool()) {
				auto *content = dialog->findChild<QWidget *>(QStringLiteral("adaptiveDialogContent"));
				auto desired = dialog->property("meetingUiPreferredSize").toSize();
				if (content) desired = desired.expandedTo(content->sizeHint());
				if (auto *screen = screenForWindow(*dialog)) {
					desired = desired.boundedTo(screen->availableGeometry().size() - QSize(32, 64));
				}
				dialog->resize(desired);
			}
			applyNativeCorners(*dialog, tone);
			if (!dialog->property(kCenteredProperty).toBool()) {
				centerOnParentOrScreen(*dialog);
				dialog->setProperty(kCenteredProperty, true);
			}
		}
		return QObject::eventFilter(watched, event);
	}
private:
	bool stylingChoice_ = false;
};

class ThemeRuntime final : public QObject, public QAbstractNativeEventFilter {
public:
	ThemeRuntime(QApplication &application, InstallOptions options)
		: QObject(&application), application_(application), options_(std::move(options)) {
		for (const auto *path : {":/meeting-ui/styles/widgets.qss", ":/meeting-ui/styles/application.qss"}) {
			QFile file(QString::fromLatin1(path));
			if (!file.open(QIODevice::ReadOnly)) qFatal("Cannot load the application theme resource");
			sheet_ += QString::fromUtf8(file.readAll()) + QLatin1Char('\n');
		}
		const auto saved = QSettings().value(QString::fromLatin1(kThemeSetting), QStringLiteral("system")).toString();
		selected = themeFromId(saved);
		if (saved != themeId(selected)) QSettings().setValue(QString::fromLatin1(kThemeSetting), themeId(selected));
		probeSystem();
		effective = selected == Theme::System ? system_ : selected;
		application_.installNativeEventFilter(this);
		application_.installEventFilter(this);
	}
	~ThemeRuntime() override { application_.removeNativeEventFilter(this); }

	void setSelection(Theme value) {
		value = themeFromId(themeId(value));
		const bool selectionChanged = selected != value;
		selected = value;
		QSettings().setValue(QString::fromLatin1(kThemeSetting), themeId(selected));
		if (selected == Theme::System) probeSystem();
		apply(selectionChanged);
	}
	void refreshSystem() {
		probeSystem();
		if (selected == Theme::System) apply(false);
	}
	void apply(bool notifySelection = false, bool initial = false) {
		const auto next = selected == Theme::System ? system_ : selected;
		if (applying_) {
			pendingNotify_ = pendingNotify_ || notifySelection;
			if (!applyQueued_) {
				applyQueued_ = true;
				QTimer::singleShot(0, this, [this] {
					applyQueued_ = false;
					const auto notify = std::exchange(pendingNotify_, false);
					apply(notify);
				});
			}
			return;
		}
		QScopedValueRollback<bool> guard(applying_, true);
		if (!initial && effective == next) {
			if (notifySelection) notifyObservers();
			return;
		}
		effective = next;
		++generation;
		std::vector<QPointer<QWidget>> widgets;
		for (auto *widget : QApplication::allWidgets()) widgets.emplace_back(widget);
		for (const auto &widget : widgets) if (widget) widget->setProperty(kToneProperty, toneName(currentTone()));
		application_.setPalette(applicationPalette());
		application_.setStyleSheet(renderSheet(sheet_));
		for (const auto &widget : widgets) {
			if (!widget) continue;
			// QSS also sets WA_SetPalette. Only replace palettes owned by this API;
			// overriding every explicit palette would undo freshly polished menu colors.
			if (widget->property(kSurfacePaletteProperty).toBool()) applySurfacePalette(*widget, currentTone());
			if (auto *combo = qobject_cast<QComboBox *>(widget.data())) styleChoiceControls(*combo, currentTone());
			if (widget->isWindow() && widget->isVisible()) applyNativeCorners(*widget, currentTone());
			widget->update();
		}
		notifyObservers();
	}
	void observe(QObject &context, std::function<void()> callback) {
		const auto id = ++observerId_;
		observers_.push_back({id, &context,
			std::make_shared<std::function<void()>>(std::move(callback))});
		connect(&context, &QObject::destroyed, this, [this, id] {
			std::erase_if(observers_, [id](const Observer &entry) { return entry.id == id; });
		});
	}
	Theme selected = Theme::System;
	Theme effective = Theme::Light;
	quint64 generation = 0;

protected:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	bool nativeEventFilter(const QByteArray &, void *message, qintptr *) override {
#else
	bool nativeEventFilter(const QByteArray &, void *message, long *) override {
#endif
#if defined(Q_OS_WIN)
		const auto *msg = static_cast<MSG *>(message);
		if (msg && (msg->message == WM_SETTINGCHANGE || msg->message == WM_THEMECHANGED)) scheduleProbe();
#endif
		return false;
	}
	bool eventFilter(QObject *watched, QEvent *event) override {
		if (watched == &application_ && event->type() == QEvent::ApplicationActivate) scheduleProbe();
		return QObject::eventFilter(watched, event);
	}

private:
	struct Observer {
		quint64 id;
		QPointer<QObject> context;
		std::shared_ptr<std::function<void()>> callback;
	};
	void probeSystem() {
		const auto result = options_.systemThemeProbe ? options_.systemThemeProbe() : platformTheme();
		if (result && (*result == Theme::Light || *result == Theme::Dark)) system_ = *result;
	}
	void scheduleProbe() {
		if (probeQueued_ || selected != Theme::System) return;
		probeQueued_ = true;
		QTimer::singleShot(0, this, [this] { probeQueued_ = false; refreshSystem(); });
	}
	void notifyObservers() {
		// A callback may destroy another subscriber or create a new one.
		const auto snapshot = observers_;
		for (const auto &entry : snapshot) if (entry.context) (*entry.callback)();
	}
	QApplication &application_;
	InstallOptions options_;
	QString sheet_;
	Theme system_ = Theme::Light;
	std::vector<Observer> observers_;
	quint64 observerId_ = 0;
	bool probeQueued_ = false, applying_ = false, applyQueued_ = false, pendingNotify_ = false;
};

} // namespace

void configureModelessWindow(QDialog &dialog) {
	dialog.setWindowFlags(
		Qt::Window
		| Qt::WindowTitleHint
		| Qt::WindowSystemMenuHint
		| Qt::WindowMinMaxButtonsHint
		| Qt::WindowCloseButtonHint);
	dialog.setModal(false);
	dialog.setWindowModality(Qt::NonModal);
}

void makeDialogAdaptive(QDialog &dialog, QSize preferredSize) {
	if (!dialog.layout() || dialog.property("meetingUiAdaptive").toBool()) return;
	dialog.setProperty("meetingUiAdaptive", true);
	for (auto *label : dialog.findChildren<QLabel *>()) {
		if (label->maximumHeight() == QWIDGETSIZE_MAX) label->setWordWrap(true);
	}
	for (auto *form : dialog.findChildren<QFormLayout *>()) {
		form->setRowWrapPolicy(QFormLayout::WrapLongRows);
		form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	}
	auto *content = new QWidget;
	content->setObjectName(QStringLiteral("adaptiveDialogContent"));
	content->setLayout(dialog.layout());
	content->layout()->setSizeConstraint(QLayout::SetMinAndMaxSize);
	auto *scroll = new QScrollArea(&dialog);
	scroll->setObjectName(QStringLiteral("adaptiveDialogScroll"));
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidgetResizable(true);
	scroll->viewport()->setAutoFillBackground(false);
	content->setAutoFillBackground(false);
	scroll->setWidget(content);
	auto *root = new QVBoxLayout(&dialog);
	root->setContentsMargins(0, 0, 0, 0);
	root->addWidget(scroll);
	dialog.setMinimumSize(280, 180);
	dialog.setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
	dialog.setSizeGripEnabled(!dialog.windowFlags().testFlag(Qt::FramelessWindowHint));
	dialog.setProperty("meetingUiPreferredSize", preferredSize);
	dialog.resize(preferredSize);
}

void install(QApplication &application, InstallOptions options) {
	if (application.property(kInstalledProperty).toBool()) {
		return;
	}
	Q_ASSERT(QThread::currentThread() == application.thread());
	runtime = new ThemeRuntime(application, std::move(options));
	application.setProperty(kInstalledProperty, true);
	application.installEventFilter(new ThemeEventFilter(&application));
	runtime->apply(false, true);
}

QString themeId(Theme theme) {
	switch (theme) {
	case Theme::Light: return QStringLiteral("light");
	case Theme::Dark: return QStringLiteral("dark");
	case Theme::Teal: return QStringLiteral("teal");
	default: return QStringLiteral("system");
	}
}

Theme themeFromId(const QString &id) {
	if (id == QStringLiteral("light")) return Theme::Light;
	if (id == QStringLiteral("dark")) return Theme::Dark;
	if (id == QStringLiteral("teal")) return Theme::Teal;
	return Theme::System;
}

Theme selectedTheme() { return runtime ? runtime->selected : Theme::System; }
Theme effectiveTheme() { return runtime ? runtime->effective : Theme::Light; }
quint64 revision() { return runtime ? runtime->generation : 0; }
QColor color(ColorRole role) {
	const auto index = static_cast<size_t>(role);
	const auto &colors = themeColors(effectiveTheme());
	Q_ASSERT(index < colors.size());
	return QColor(QString::fromLatin1(colors[index < colors.size() ? index : 5]));
}

void setTheme(Theme theme) {
	Q_ASSERT(!qApp || QThread::currentThread() == qApp->thread());
	if (runtime) runtime->setSelection(theme);
}

void refreshSystemTheme() {
	Q_ASSERT(!qApp || QThread::currentThread() == qApp->thread());
	if (runtime) runtime->refreshSystem();
}

void observeChanges(QObject &context, std::function<void()> callback) {
	Q_ASSERT(!qApp || (QThread::currentThread() == qApp->thread() && context.thread() == qApp->thread()));
	if (runtime) runtime->observe(context, std::move(callback));
}

void setTone(QWidget &widget, Tone tone) {
	tone = resolvedTone(tone);
	widget.setProperty(kToneProperty, toneName(tone));
	refreshStyle(widget);
}

void applyChoiceSurface(QWidget &surface, Tone tone) {
	tone = resolvedTone(tone);
	auto palette = installed() ? applicationPalette() : surface.palette();
	const auto light = tone == Tone::Light;
	if (!installed()) {
		palette.setColor(QPalette::Window, light ? QColor("#ffffff") : QColor("#242831"));
		palette.setColor(QPalette::WindowText, light ? QColor("#1f2329") : QColor("#f3f4f6"));
		palette.setColor(QPalette::Base, light ? QColor("#ffffff") : QColor("#242831"));
		palette.setColor(QPalette::AlternateBase, light ? QColor("#f7f8fa") : QColor("#2b303a"));
		palette.setColor(QPalette::Text, light ? QColor("#1f2329") : QColor("#f3f4f6"));
		palette.setColor(QPalette::Highlight, light ? QColor("#eaf3ff") : QColor("#394252"));
		palette.setColor(QPalette::HighlightedText, light ? QColor("#1677ff") : QColor("#ffffff"));
	} else {
		palette.setColor(QPalette::Window, color(ColorRole::Surface));
	}
	surface.setProperty(kSurfacePaletteProperty, true);
	setTone(surface, tone);
	surface.setPalette(palette);
	surface.setAutoFillBackground(true);
}

void applySurfacePalette(QWidget &widget, Tone tone) {
	tone = resolvedTone(tone);
	applyChoiceSurface(widget, tone);
	auto palette = widget.palette();
	palette.setColor(QPalette::Link, installed() ? color(ColorRole::Accent)
		: tone == Tone::Dark ? QColor("#65b5ff") : QColor("#1677ff"));
	palette.setColor(QPalette::LinkVisited, installed() ? color(ColorRole::Success)
		: tone == Tone::Dark ? QColor("#8edbc2") : QColor("#267d66"));
	// QStyleSheetStyle can restore platform Link roles during polish. Custom
	// painters use these theme tokens independently of that palette rewrite.
	widget.setProperty("chartPrimaryColor", palette.color(QPalette::Link));
	widget.setProperty("chartSecondaryColor", palette.color(QPalette::LinkVisited));
	widget.setPalette(palette);
}

void styleChoiceControls(QWidget &widget, Tone tone) {
	tone = resolvedTone(tone);
	setTone(widget, tone);
	auto choices = widget.findChildren<QComboBox *>();
	if (auto *choice = qobject_cast<QComboBox *>(&widget)) {
		choices.push_front(choice);
	}
	for (auto *choice : choices) {
		setTone(*choice, tone);
		auto *view = choice->view();
		if (!view || !view->viewport()) continue;
		applyChoiceSurface(*view, tone);
		applyChoiceSurface(*view->viewport(), tone);
		// Qt reparents the list beneath a private Qt::Popup container. Style that
		// container directly so a dark meeting-window ancestor cannot leak in.
		auto *popup = view->window();
		if (popup && popup != choice->window()
				&& popup->windowFlags().testFlag(Qt::Popup)) {
			applyChoiceSurface(*popup, tone);
		}
	}
}

void styleMenu(QMenu &menu, Tone tone) {
	tone = resolvedTone(tone);
	menu.setProperty(kToneProperty, toneName(tone));
	menu.setProperty(kMenuStyledToneProperty, toneName(tone));
#if !defined(Q_OS_WIN)
	menu.setAttribute(Qt::WA_TranslucentBackground, true);
#endif
	refreshStyle(menu);
}

void centerOnScreen(QWidget &window) {
	auto *screen = screenForWindow(window);
	if (!screen) {
		return;
	}
	const auto available = screen->availableGeometry();
	auto size = window.frameGeometry().size();
	if (size.isEmpty()) {
		size = window.size();
	}
	const auto x = available.x() + qMax(0, (available.width() - size.width()) / 2);
	const auto y = available.y() + qMax(0, (available.height() - size.height()) / 2);
	window.move(x, y);
}

} // namespace MeetingUI::AppTheme
